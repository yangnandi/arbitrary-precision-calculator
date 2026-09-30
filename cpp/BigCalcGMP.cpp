// BigCalcGMP - exact arbitrary-precision calculator built on GMP.
//
// Mirrors the Java BigCalc exactly: every value is a canonical exact
// rational (mpq_class), so there is no digit limit and no rounding inside
// evaluation. Irrational quantities are only produced at a caller-chosen
// precision and are returned as exact rationals.
//
// Build:
//   C:\msys64\mingw64\bin\g++.exe -O2 -std=c++20 -static -o bigcalcgmp.exe BigCalcGMP.cpp -lgmpxx -lgmp
//
// Keep this file ASCII-only.

#include <gmpxx.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#endif

using BigInt = mpz_class;
using Rat = mpq_class;

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// =====================================================================
//  errors
// =====================================================================

struct CalcError : std::runtime_error {
    explicit CalcError(const std::string& m) : std::runtime_error(m) {}
};

// =====================================================================
//  rational helpers
// =====================================================================

static inline bool isInteger(const Rat& q) { return q.get_den() == 1; }

// GMP gotcha: mpq_class(num, den) does NOT reduce the fraction. Every
// construction from a numerator/denominator pair must be canonicalised,
// otherwise get_den() != 1 for plain integers and printed results differ
// from the Java implementation.
static Rat ratFrom(const BigInt& num, const BigInt& den) {
    if (den == 0) throw CalcError("division by zero");
    Rat r(num, den);
    r.canonicalize();
    return r;
}

static BigInt floorQ(const Rat& q) {
    BigInt r;
    mpz_fdiv_q(r.get_mpz_t(), q.get_num().get_mpz_t(), q.get_den().get_mpz_t());
    return r;
}

static BigInt ceilQ(const Rat& q) {
    BigInt r;
    mpz_cdiv_q(r.get_mpz_t(), q.get_num().get_mpz_t(), q.get_den().get_mpz_t());
    return r;
}

static BigInt truncQ(const Rat& q) { return q.get_num() / q.get_den(); }

// =====================================================================
//  tokenizer
// =====================================================================

enum class T { NUM, IDENT, OP, LPAREN, RPAREN, COMMA, END };

struct Tok {
    T type;
    std::string text;
    BigInt num;
};

static bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static std::vector<Tok> tokenize(const std::string& src) {
    std::vector<Tok> out;
    size_t i = 0, len = src.size();
    while (i < len) {
        char c = src[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t start = i;
            int base = 10;
            if (c == '0' && i + 1 < len && (src[i + 1] == 'x' || src[i + 1] == 'X')) { base = 16; i += 2; }
            else if (c == '0' && i + 1 < len && (src[i + 1] == 'b' || src[i + 1] == 'B')) { base = 2; i += 2; }
            else if (c == '0' && i + 1 < len && (src[i + 1] == 'o' || src[i + 1] == 'O')) { base = 8; i += 2; }
            size_t digitsStart = i;
            std::string digits;
            while (i < len) {
                char d = src[i];
                if (d == '_') { ++i; continue; }
                if (base == 16 ? isHexDigit(d)
                               : (base == 8 ? (d >= '0' && d <= '7')
                                            : (base == 2 ? (d == '0' || d == '1')
                                                         : (d >= '0' && d <= '9')))) {
                    digits.push_back(d);
                    ++i;
                }
                else break;
            }
            (void)digitsStart;
            if (digits.empty()) throw CalcError("malformed numeric literal '" + src.substr(start) + "'");
            BigInt v;
            if (mpz_set_str(v.get_mpz_t(), digits.c_str(), base) != 0) {
                throw CalcError("malformed numeric literal '" + src.substr(start, i - start) + "'");
            }
            out.push_back(Tok{T::NUM, src.substr(start, i - start), v});
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t start = i;
            while (i < len && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_')) ++i;
            out.push_back(Tok{T::IDENT, src.substr(start, i - start), BigInt()});
            continue;
        }
        switch (c) {
            case '(': out.push_back(Tok{T::LPAREN, "(", BigInt()}); ++i; continue;
            case ')': out.push_back(Tok{T::RPAREN, ")", BigInt()}); ++i; continue;
            case ',': out.push_back(Tok{T::COMMA, ",", BigInt()}); ++i; continue;
            case '+': case '-': case '*': case '/': case '%': case '^': case '!': case '=':
                out.push_back(Tok{T::OP, std::string(1, c), BigInt()}); ++i; continue;
            default:
                throw CalcError(std::string("unexpected character '") + c + "'");
        }
    }
    out.push_back(Tok{T::END, "", BigInt()});
    return out;
}

static std::string tokenText(const Tok& t) {
    return t.type == T::END ? std::string("end of input") : t.text;
}

// =====================================================================
//  engine
// =====================================================================

class Engine {
public:
    std::map<std::string, Rat> vars;
    Rat last;
    int digits = 30;
    int base = 10;
    bool frac = false;
    bool timing = false;

    Engine() : last(0) {}

    Rat evaluate(const std::string& line);

    // ---------------- integer algorithms ----------------
    static BigInt productRange(unsigned long lo, unsigned long hi) {
        if (lo > hi) return BigInt(1);
        if (lo == hi) return BigInt(lo);
        if (hi - lo == 1) return BigInt(lo) * BigInt(hi);
        unsigned long mid = lo + (hi - lo) / 2;
        return productRange(lo, mid) * productRange(mid + 1, hi);
    }

    static BigInt factorial(const BigInt& n) {
        if (n < 0) throw CalcError("factorial requires a non-negative integer");
        if (!mpz_fits_ulong_p(n.get_mpz_t())) throw CalcError("factorial argument is astronomically large");
        unsigned long k = n.get_ui();
        return k < 2 ? BigInt(1) : productRange(2, k);
    }

    static BigInt fibonacci(const BigInt& n) {
        if (n < 0) throw CalcError("fib requires a non-negative integer");
        if (!mpz_fits_ulong_p(n.get_mpz_t())) throw CalcError("fib argument is too large");
        unsigned long k = n.get_ui();
        if (k == 0) return BigInt(0);
        BigInt a = 0, b = 1;
        int bits = static_cast<int>(mpz_sizeinbase(n.get_mpz_t(), 2));
        for (int i = bits - 1; i >= 0; --i) {
            BigInt d = a * (2 * b - a);
            BigInt e = a * a + b * b;
            if (mpz_tstbit(n.get_mpz_t(), static_cast<mp_bitcnt_t>(i))) { a = e; b = d + e; }
            else { a = d; b = e; }
        }
        return a;
    }

    static bool exactRoot(const Rat& v, unsigned long k, Rat& out) {
        if (k == 0) throw CalcError("root index must be >= 1");
        bool negative = v < 0;
        if (negative && (k % 2 == 0)) return false;
        BigInt num = v.get_num();
        if (num < 0) num = -num;
        BigInt den = v.get_den();
        BigInt rn, rd;
        if (!mpz_root(rn.get_mpz_t(), num.get_mpz_t(), k)) return false;
        if (!mpz_root(rd.get_mpz_t(), den.get_mpz_t(), k)) return false;
        out = ratFrom(rn, rd);
        if (negative) out = -out;
        return true;
    }

    static Rat power(const Rat& base, const Rat& exponent) {
        if (isInteger(exponent)) {
            BigInt e = exponent.get_num();
            if (!mpz_fits_slong_p(e.get_mpz_t())) throw CalcError("exponent is too large to materialise");
            long k = e.get_si();
            if (k == 0) return Rat(1);
            if (base == 0 && k < 0) throw CalcError("division by zero (0 raised to a negative power)");
            unsigned long m = k < 0 ? static_cast<unsigned long>(-(k + 1)) + 1UL
                                    : static_cast<unsigned long>(k);
            BigInt bn = base.get_num(), bd = base.get_den();
            BigInt rn, rd;
            mpz_pow_ui(rn.get_mpz_t(), bn.get_mpz_t(), m);
            mpz_pow_ui(rd.get_mpz_t(), bd.get_mpz_t(), m);
            return k < 0 ? ratFrom(rd, rn) : ratFrom(rn, rd);
        }
        BigInt p = exponent.get_num();
        BigInt q = exponent.get_den();
        if (!mpz_fits_ulong_p(q.get_mpz_t())) throw CalcError("root index is too large");
        unsigned long k = q.get_ui();
        Rat root;
        if (!exactRoot(base, k, root)) {
            throw CalcError("result is irrational; use root(x, k, digits) or sqrt(x, digits)");
        }
        if (!mpz_fits_slong_p(p.get_mpz_t())) throw CalcError("exponent is too large to materialise");
        long kp = p.get_si();
        if (kp == 0) return Rat(1);
        if (root == 0 && kp < 0) throw CalcError("division by zero");
        unsigned long m = kp < 0 ? static_cast<unsigned long>(-(kp + 1)) + 1UL
                                 : static_cast<unsigned long>(kp);
        BigInt rn = root.get_num(), rd = root.get_den();
        BigInt pn, pd;
        mpz_pow_ui(pn.get_mpz_t(), rn.get_mpz_t(), m);
        mpz_pow_ui(pd.get_mpz_t(), rd.get_mpz_t(), m);
        return kp < 0 ? ratFrom(pd, pn) : ratFrom(pn, pd);
    }

    // ---------------- decimal rendering ----------------
    static bool terminates(const Rat& v) {
        BigInt t = v.get_den();
        while (mpz_even_p(t.get_mpz_t())) t >>= 1;
        while (mpz_divisible_ui_p(t.get_mpz_t(), 5UL)) t /= 5;
        return t == 1;
    }

    static std::string exactDecimal(const Rat& v) {
        bool negative = v < 0;
        BigInt num = v.get_num();
        if (num < 0) num = -num;
        BigInt den = v.get_den();
        int twos = 0, fives = 0;
        BigInt t = den;
        while (mpz_even_p(t.get_mpz_t())) { t >>= 1; ++twos; }
        while (mpz_divisible_ui_p(t.get_mpz_t(), 5UL)) { t /= 5; ++fives; }
        int scale = std::max(twos, fives);
        BigInt pow10;
        mpz_ui_pow_ui(pow10.get_mpz_t(), 10UL, static_cast<unsigned long>(scale));
        BigInt scaledNum = (num * pow10) / den;
        std::string digits = scaledNum.get_str();
        if (static_cast<int>(digits.size()) <= scale) {
            digits = std::string(scale - digits.size(), '0') + digits;
        }
        int cut = static_cast<int>(digits.size()) - scale;
        std::string head = cut == 0 ? std::string("0") : digits.substr(0, cut);
        std::string tail = digits.substr(cut);
        while (!tail.empty() && tail.back() == '0') tail.pop_back();
        return (negative ? "-" : "") + head + (tail.empty() ? "" : "." + tail);
    }

    static std::string toDecimal(const Rat& v, int scale) {
        bool negative = v < 0;
        BigInt num = v.get_num();
        if (num < 0) num = -num;
        BigInt den = v.get_den();
        BigInt pow10;
        mpz_ui_pow_ui(pow10.get_mpz_t(), 10UL, static_cast<unsigned long>(scale));
        BigInt scaled = (num * pow10) / den;
        BigInt rem = (num * pow10) % den;
        if (2 * rem >= den) scaled += 1;
        std::string digits = scaled.get_str();
        if (scale == 0) return (negative ? "-" : "") + digits;
        if (static_cast<int>(digits.size()) <= scale) {
            std::string body(scale - digits.size(), '0');
            body += digits;
            return (negative ? "-" : "") + std::string("0.") + body;
        }
        int cut = static_cast<int>(digits.size()) - scale;
        return (negative ? "-" : "") + digits.substr(0, cut) + "." + digits.substr(cut);
    }

    // ---------------- pi / e ----------------
    static const BigInt& c3Over24() {
        static const BigInt value = [] {
            BigInt c;
            mpz_ui_pow_ui(c.get_mpz_t(), 640320UL, 3UL);
            c /= 24;
            return c;
        }();
        return value;
    }

    static void chudnovsky(unsigned long a, unsigned long b, BigInt& P, BigInt& Q, BigInt& T) {
        if (b - a == 1) {
            BigInt pab, qab;
            if (a == 0) { pab = 1; qab = 1; }
            else {
                BigInt ai(a);
                pab = (6 * ai - 5) * (2 * ai - 1) * (6 * ai - 1);
                qab = ai * ai * ai * c3Over24();
            }
            BigInt tab = pab * (BigInt(13591409) + BigInt(545140134) * BigInt(a));
            if (a & 1UL) tab = -tab;
            P = pab; Q = qab; T = tab;
            return;
        }
        unsigned long m = a + (b - a) / 2;
        BigInt P1, Q1, T1, P2, Q2, T2;
        chudnovsky(a, m, P1, Q1, T1);
        chudnovsky(m, b, P2, Q2, T2);
        P = P1 * P2;
        Q = Q1 * Q2;
        T = Q2 * T1 + P1 * T2;
    }

    static Rat piRat(int digits) {
        if (digits < 0) throw CalcError("digits must be >= 0");
        const int guard = 15;
        unsigned long work = static_cast<unsigned long>(digits + guard);
        unsigned long terms = static_cast<unsigned long>(work / 14.181647462725477) + 2;
        BigInt P, Q, T;
        chudnovsky(0, terms, P, Q, T);
        BigInt scale;
        mpz_ui_pow_ui(scale.get_mpz_t(), 10UL, work);
        BigInt radicand = BigInt(10005) * scale * scale;
        BigInt s;
        mpz_sqrt(s.get_mpz_t(), radicand.get_mpz_t());
        BigInt scaledPi = (BigInt(426880) * Q * s) / T;

        BigInt div;
        mpz_ui_pow_ui(div.get_mpz_t(), 10UL, static_cast<unsigned long>(guard));
        BigInt q = scaledPi / div, r = scaledPi % div;
        if (2 * r >= div) q += 1;

        BigInt den;
        mpz_ui_pow_ui(den.get_mpz_t(), 10UL, static_cast<unsigned long>(digits));
        return ratFrom(q, den);
    }

    static Rat eRat(int digits) {
        if (digits < 0) throw CalcError("digits must be >= 0");
        const int guard = 15;
        unsigned long work = static_cast<unsigned long>(digits + guard);

        BigInt target;
        mpz_ui_pow_ui(target.get_mpz_t(), 10UL, work + 2);
        unsigned long N = 1;
        BigInt factN = 1;
        while (factN < target) { ++N; factN *= N; }

        // A = sum_{k=0}^{N} N!/k!   ->   e ~ A / N!
        BigInt A = 1, t = 1;
        for (unsigned long k = N; k >= 1; --k) { t *= k; A += t; }

        BigInt scale;
        mpz_ui_pow_ui(scale.get_mpz_t(), 10UL, work);
        BigInt scaledE = (A * scale) / factN;

        BigInt div;
        mpz_ui_pow_ui(div.get_mpz_t(), 10UL, static_cast<unsigned long>(guard));
        BigInt q = scaledE / div, r = scaledE % div;
        if (2 * r >= div) q += 1;

        BigInt den;
        mpz_ui_pow_ui(den.get_mpz_t(), 10UL, static_cast<unsigned long>(digits));
        return ratFrom(q, den);
    }

    static Rat rootApprox(const Rat& v, unsigned long k, int digits) {
        if (k == 0) throw CalcError("root index must be >= 1");
        if (digits < 0) throw CalcError("digits must be >= 0");
        bool negative = v < 0;
        if (negative && (k % 2 == 0)) throw CalcError("even root of a negative number is not real");
        BigInt num = v.get_num();
        if (num < 0) num = -num;
        BigInt den = v.get_den();
        BigInt scale;
        mpz_ui_pow_ui(scale.get_mpz_t(), 10UL, static_cast<unsigned long>(digits));
        BigInt scaleK;
        mpz_pow_ui(scaleK.get_mpz_t(), scale.get_mpz_t(), k);
        BigInt inner = (num * scaleK) / den;
        BigInt root;
        mpz_root(root.get_mpz_t(), inner.get_mpz_t(), k);
        Rat r = ratFrom(root, scale);
        return negative ? -r : r;
    }

    // ---------------- function dispatch ----------------
    Rat call(const std::string& name, const std::vector<Rat>& a);

private:
    Rat modulo(const Rat& x, const Rat& y) {
        if (y == 0) throw CalcError("division by zero");
        return x - y * Rat(floorQ(x / y));
    }
    friend class Parser;
};

// =====================================================================
//  parser
// =====================================================================

class Parser {
public:
    Parser(std::vector<Tok> toks, Engine& engine) : toks_(std::move(toks)), engine_(engine) {}

    Rat statement() {
        const Tok& t = peek();
        if (t.type == T::IDENT && pos_ + 1 < toks_.size()
                && toks_[pos_ + 1].type == T::OP && toks_[pos_ + 1].text == "=") {
            std::string name = t.text;
            pos_ += 2;
            Rat value = statement();
            engine_.vars[name] = value;
            return value;
        }
        return additive();
    }

    const Tok& peek() const { return toks_[pos_]; }

private:
    std::vector<Tok> toks_;
    Engine& engine_;
    size_t pos_ = 0;

    bool acceptOp(const std::string& op) {
        if (peek().type == T::OP && peek().text == op) { ++pos_; return true; }
        return false;
    }
    bool accept(T type) {
        if (peek().type == type) { ++pos_; return true; }
        return false;
    }
    void expect(T type, const std::string& what) {
        if (!accept(type)) throw CalcError("expected " + what + ", found " + tokenText(peek()));
    }

    Rat additive() {
        Rat left = multiplicative();
        while (true) {
            if (acceptOp("+")) left = left + multiplicative();
            else if (acceptOp("-")) left = left - multiplicative();
            else return left;
        }
    }

    Rat modulo(const Rat& x, const Rat& y) {
        if (y == 0) throw CalcError("division by zero");
        return x - y * Rat(floorQ(x / y));
    }

    Rat multiplicative() {
        Rat left = unary();
        while (true) {
            if (acceptOp("*")) left = left * unary();
            else if (acceptOp("/")) {
                Rat rhs = unary();
                if (rhs == 0) throw CalcError("division by zero");
                left = left / rhs;
            }
            else if (acceptOp("%")) left = modulo(left, unary());
            else return left;
        }
    }

    Rat unary() {
        if (acceptOp("-")) return -unary();
        if (acceptOp("+")) return unary();
        return power();
    }

    Rat power() {
        Rat base = postfix();
        if (acceptOp("^")) {
            Rat exponent = unary();
            return Engine::power(base, exponent);
        }
        return base;
    }

    Rat postfix() {
        Rat value = atom();
        while (acceptOp("!")) {
            if (!isInteger(value)) throw CalcError("factorial requires an integer");
            value = Rat(Engine::factorial(value.get_num()));
        }
        return value;
    }

    Rat atom() {
        const Tok& t = peek();
        switch (t.type) {
            case T::NUM: {
                Rat v(t.num);
                ++pos_;
                return v;
            }
            case T::LPAREN: {
                ++pos_;
                Rat v = statement();
                expect(T::RPAREN, "')'");
                return v;
            }
            case T::IDENT: {
                std::string name = t.text;
                ++pos_;
                if (accept(T::LPAREN)) {
                    std::vector<Rat> args;
                    if (!accept(T::RPAREN)) {
                        do { args.push_back(statement()); } while (accept(T::COMMA));
                        expect(T::RPAREN, "')'");
                    }
                    return engine_.call(name, args);
                }
                if (name == "ans") return engine_.last;
                auto it = engine_.vars.find(name);
                if (it == engine_.vars.end()) throw CalcError("undefined variable '" + name + "'");
                return it->second;
            }
            default:
                throw CalcError("unexpected " + tokenText(t));
        }
    }
};

Rat Engine::evaluate(const std::string& line) {
    Parser p(tokenize(line), *this);
    Rat v = p.statement();
    if (p.peek().type != T::END) {
        throw CalcError("unexpected trailing input: " + tokenText(p.peek()));
    }
    return v;
}

// =====================================================================
//  function dispatch
// =====================================================================

static void arity(const std::string& name, const std::vector<Rat>& a, size_t n) {
    if (a.size() != n) {
        throw CalcError(name + "() takes " + std::to_string(n) + " argument(s), got " + std::to_string(a.size()));
    }
}

static void minArity(const std::string& name, const std::vector<Rat>& a, size_t n) {
    if (a.size() < n) {
        throw CalcError(name + "() takes at least " + std::to_string(n) + " argument(s), got " + std::to_string(a.size()));
    }
}

static unsigned long asULong(const Rat& v, const std::string& what) {
    if (!isInteger(v)) throw CalcError(what + " requires an integer, got " + v.get_str());
    if (!mpz_fits_ulong_p(v.get_num().get_mpz_t())) throw CalcError(what + " is too large");
    return v.get_num().get_ui();
}

static int asInt(const Rat& v, const std::string& what) {
    unsigned long u = asULong(v, what);
    if (u > 1000000000UL) throw CalcError(what + " is unreasonably large");
    return static_cast<int>(u);
}

static std::string toBaseString(const BigInt& n, int base) {
    return n.get_str(base);
}

Rat Engine::call(const std::string& name, const std::vector<Rat>& a) {
    if (name == "abs") { arity(name, a, 1); return a[0] < 0 ? -a[0] : a[0]; }
    if (name == "floor") { arity(name, a, 1); return Rat(floorQ(a[0])); }
    if (name == "ceil") { arity(name, a, 1); return Rat(ceilQ(a[0])); }
    if (name == "trunc") { arity(name, a, 1); return Rat(truncQ(a[0])); }
    if (name == "round") { arity(name, a, 1); return Rat(floorQ(a[0] + ratFrom(BigInt(1), BigInt(2)))); }
    if (name == "sign") { arity(name, a, 1); return Rat(a[0] > 0 ? 1 : (a[0] < 0 ? -1 : 0)); }

    if (name == "gcd") {
        arity(name, a, 2);
        if (!isInteger(a[0]) || !isInteger(a[1])) throw CalcError("gcd requires integers");
        BigInt r;
        mpz_gcd(r.get_mpz_t(), a[0].get_num().get_mpz_t(), a[1].get_num().get_mpz_t());
        return Rat(r);
    }
    if (name == "lcm") {
        arity(name, a, 2);
        if (!isInteger(a[0]) || !isInteger(a[1])) throw CalcError("lcm requires integers");
        BigInt r;
        mpz_lcm(r.get_mpz_t(), a[0].get_num().get_mpz_t(), a[1].get_num().get_mpz_t());
        return Rat(r);
    }
    if (name == "min") {
        minArity(name, a, 1);
        Rat r = a[0];
        for (const Rat& v : a) if (v < r) r = v;
        return r;
    }
    if (name == "max") {
        minArity(name, a, 1);
        Rat r = a[0];
        for (const Rat& v : a) if (v > r) r = v;
        return r;
    }

    if (name == "fact") {
        arity(name, a, 1);
        if (!isInteger(a[0])) throw CalcError("fact requires an integer");
        return Rat(factorial(a[0].get_num()));
    }
    if (name == "fib") {
        arity(name, a, 1);
        if (!isInteger(a[0])) throw CalcError("fib requires an integer");
        return Rat(fibonacci(a[0].get_num()));
    }

    if (name == "isqrt") {
        arity(name, a, 1);
        if (!isInteger(a[0])) throw CalcError("isqrt requires an integer");
        if (a[0] < 0) throw CalcError("isqrt requires a non-negative integer");
        BigInt r;
        mpz_sqrt(r.get_mpz_t(), a[0].get_num().get_mpz_t());
        return Rat(r);
    }

    if (name == "ndigits") {
        arity(name, a, 1);
        if (!isInteger(a[0])) throw CalcError("ndigits requires an integer");
        BigInt n = a[0].get_num();
        if (n < 0) n = -n;
        if (n == 0) return Rat(1);
        size_t d = mpz_sizeinbase(n.get_mpz_t(), 10);   // may overestimate by 1
        BigInt p;
        mpz_ui_pow_ui(p.get_mpz_t(), 10UL, static_cast<unsigned long>(d - 1));
        if (n < p) --d;
        return Rat(BigInt(static_cast<unsigned long>(d)));
    }

    if (name == "isprime") {
        arity(name, a, 1);
        if (!isInteger(a[0])) throw CalcError("isprime requires an integer");
        BigInt n = a[0].get_num();
        if (n < 2) return Rat(0);
        return Rat(mpz_probab_prime_p(n.get_mpz_t(), 25) != 0 ? 1 : 0);
    }

    if (name == "nextprime") {
        arity(name, a, 1);
        if (!isInteger(a[0])) throw CalcError("nextprime requires an integer");
        BigInt n = a[0].get_num();
        BigInt r;
        mpz_nextprime(r.get_mpz_t(), n.get_mpz_t());
        return Rat(r);
    }

    if (name == "pow") {
        arity(name, a, 2);
        return power(a[0], a[1]);
    }

    if (name == "sqrt" || name == "cbrt") {
        unsigned long k = (name == "sqrt") ? 2UL : 3UL;
        if (a.size() == 1) {
            Rat r;
            if (!exactRoot(a[0], k, r)) {
                throw CalcError(name + "(x) is irrational here; use " + name + "(x, digits)");
            }
            return r;
        }
        arity(name, a, 2);
        return rootApprox(a[0], k, asInt(a[1], "digits"));
    }

    if (name == "root") {
        if (a.size() == 2) {
            unsigned long k = asULong(a[1], "root index");
            Rat r;
            if (!exactRoot(a[0], k, r)) {
                throw CalcError("root(x, k) is irrational here; use root(x, k, digits)");
            }
            return r;
        }
        arity(name, a, 3);
        return rootApprox(a[0], asULong(a[1], "root index"), asInt(a[2], "digits"));
    }

    if (name == "pi") { arity(name, a, 1); return piRat(asInt(a[0], "digits")); }
    if (name == "e") { arity(name, a, 1); return eRat(asInt(a[0], "digits")); }

    throw CalcError("unknown function '" + name + "'");
}

// =====================================================================
//  REPL
// =====================================================================

static const char* HELP =
    "BigCalcGMP 1.0 - exact arbitrary-precision calculator (no digit limit, GMP backend)\n"
    "\n"
    "Operators     + - * / % ^ !   and parentheses; ^ is right associative\n"
    "               % is a - b*floor(a/b); ! is postfix factorial\n"
    "Literals      decimal, 0x.. hex, 0b.. binary, 0o.. octal, _ separators\n"
    "Variables     x = <expr>   (also: ans = previous result)\n"
    "Exact funcs   abs floor ceil trunc round sign gcd lcm min max\n"
    "              fact fib isqrt ndigits isprime nextprime pow root\n"
    "Precision     sqrt(x, d)  root(x, k, d)  pi(d)  e(d)\n"
    "              (sqrt/root without d fail unless the result is exact)\n"
    "\n"
    "Commands\n"
    "  :digits N   decimals shown for non-integers (default 30, 0 = exact only)\n"
    "  :base N     integer output base: 10, 16, 8 or 2\n"
    "  :hex :dec :bin :oct\n"
    "  :vars       list variables\n"
    "  :frac       force n/d display for non-integers\n"
    "  :time       toggle evaluation timing\n"
    "  :help       this text\n"
    "  :quit       exit\n"
    "\n"
    "Every value is an exact rational: 1/3 stays 1/3, 2^1000000 is exact,\n"
    "100000! is exact, and results are never truncated.";

static void printValue(Engine& engine, const Rat& v) {
    if (isInteger(v)) {
        std::cout << toBaseString(v.get_num(), engine.base) << '\n';
        return;
    }
    if (!engine.frac && engine.base == 10 && Engine::terminates(v)) {
        std::cout << Engine::exactDecimal(v) << '\n';
        return;
    }
    std::cout << v.get_str() << '\n';
    if (engine.digits > 0 && engine.base == 10) {
        std::cout << "  = " << Engine::toDecimal(v, engine.digits)
                  << "  (rounded to " << engine.digits << " decimals)\n";
    }
}

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    Engine engine;
    bool interactive = false;
#if defined(_WIN32)
    interactive = _isatty(_fileno(stdin)) != 0;
#else
    interactive = isatty(fileno(stdin)) != 0;
#endif

    if (interactive) {
        std::cout << "BigCalcGMP 1.0  -  exact arbitrary-precision calculator (GMP)\n"
                     "type :help for help, :quit to exit\n";
    }

    std::string line;
    while (true) {
        if (interactive) { std::cout << "> " << std::flush; }
        if (!std::getline(std::cin, line)) break;

        line = trim(line);
        if (line.empty()) continue;

        if (line[0] == ':') {
            std::string rest = trim(line.substr(1));
            size_t sp = rest.find_first_of(" \t");
            std::string cmd = lower(sp == std::string::npos ? rest : rest.substr(0, sp));
            std::string arg = trim(sp == std::string::npos ? std::string() : rest.substr(sp + 1));
            try {
                if (cmd == "q" || cmd == "quit" || cmd == "exit") return 0;
                else if (cmd == "h" || cmd == "help") std::cout << HELP << '\n';
                else if (cmd == "d" || cmd == "digits") {
                    if (arg.empty()) throw CalcError("command :" + cmd + " needs an argument");
                    int d = std::stoi(arg);
                    if (d < 0) throw CalcError("digits must be >= 0");
                    if (d > 10000000) throw CalcError("digits is unreasonably large");
                    engine.digits = d;
                    std::cout << "digits = " << d << '\n';
                }
                else if (cmd == "base") {
                    if (arg.empty()) throw CalcError("command :" + cmd + " needs an argument");
                    int bse = std::stoi(arg);
                    if (bse != 2 && bse != 8 && bse != 10 && bse != 16) {
                        throw CalcError("base must be 2, 8, 10 or 16");
                    }
                    engine.base = bse;
                    std::cout << "base = " << bse << '\n';
                }
                else if (cmd == "hex") { engine.base = 16; std::cout << "base = 16\n"; }
                else if (cmd == "dec") { engine.base = 10; std::cout << "base = 10\n"; }
                else if (cmd == "oct") { engine.base = 8; std::cout << "base = 8\n"; }
                else if (cmd == "bin") { engine.base = 2; std::cout << "base = 2\n"; }
                else if (cmd == "vars") {
                    if (engine.vars.empty()) std::cout << "(no variables)\n";
                    else for (const auto& kv : engine.vars) {
                        std::cout << "  " << kv.first << " = " << kv.second.get_str() << '\n';
                    }
                }
                else if (cmd == "time") {
                    engine.timing = !engine.timing;
                    std::cout << "timing " << (engine.timing ? "on" : "off") << '\n';
                }
                else if (cmd == "frac") {
                    if (!arg.empty()) {
                        arg = lower(arg);
                        engine.frac = (arg == "on" || arg == "1");
                    }
                    else engine.frac = !engine.frac;
                    std::cout << "frac " << (engine.frac ? "on (exact fraction)" : "off (auto)") << '\n';
                }
                else throw CalcError("unknown command ':" + cmd + "' (try :help)");
            }
            catch (const CalcError& ex) { std::cout << "error: " << ex.what() << '\n'; }
            catch (const std::invalid_argument&) {
                std::cout << "error: command :" << cmd << " expects a number\n";
            }
            catch (const std::out_of_range&) {
                std::cout << "error: command :" << cmd << " expects a number\n";
            }
            std::cout << std::flush;
            continue;
        }

        auto start = std::chrono::steady_clock::now();
        try {
            Rat v = engine.evaluate(line);
            engine.last = v;
            printValue(engine, v);
            if (engine.timing) {
                double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count();
                std::cout << "  [" << std::fixed << std::setprecision(1) << ms << " ms]\n";
            }
        }
        catch (const CalcError& ex) { std::cout << "error: " << ex.what() << '\n'; }
        catch (const std::exception& ex) { std::cout << "error: " << ex.what() << '\n'; }
        std::cout << std::flush;
    }
    return 0;
}

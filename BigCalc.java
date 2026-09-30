import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.math.MathContext;
import java.math.RoundingMode;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * BigCalc - an exact, arbitrary-precision calculator.
 *
 * <p>Every value is an exact rational number backed by {@link BigInteger}
 * (numerator/denominator, always normalised). There is therefore no digit
 * limit and no rounding error anywhere in the evaluation: {@code 1/3} stays
 * exactly one third, {@code 2^1000000} is a million-bit integer, and
 * {@code 100000!} is computed exactly.
 *
 * <p>Irrational quantities (pi, e, non-perfect roots) are only produced when
 * the caller states how many decimal digits are wanted, and the result is
 * again an exact rational (a rounded decimal), so the value model never
 * degrades.
 *
 * <p>Performance notes:
 * <ul>
 *   <li>factorial uses binary splitting (product tree) rather than a loop;</li>
 *   <li>Fibonacci uses iterative fast doubling;</li>
 *   <li>integer square root uses {@link BigInteger#sqrt()};</li>
 *   <li>pi uses Chudnovsky with binary splitting (~14 digits per term);</li>
 *   <li>output is buffered, so multi-megabyte results print in one go.</li>
 * </ul>
 *
 * Compile:  javac -encoding utf8 BigCalc.java
 * Run:      java BigCalc            (or: java BigCalc.java, single-file mode)
 */
public final class BigCalc {

    // =================================================================
    //  errors
    // =================================================================

    static final class CalcError extends RuntimeException {
        CalcError(String message) { super(message); }
    }

    // =================================================================
    //  exact rational value
    // =================================================================

    static final class Q {
        static final Q ZERO = new Q(BigInteger.ZERO, BigInteger.ONE);
        static final Q ONE = new Q(BigInteger.ONE, BigInteger.ONE);

        final BigInteger n; // numerator, carries the sign
        final BigInteger d; // denominator, always > 0, gcd(|n|, d) == 1

        private Q(BigInteger n, BigInteger d) {
            this.n = n;
            this.d = d;
        }

        static Q of(BigInteger value) {
            return new Q(value, BigInteger.ONE);
        }

        static Q of(BigInteger num, BigInteger den) {
            if (den.signum() == 0) throw new CalcError("division by zero");
            if (den.signum() < 0) { num = num.negate(); den = den.negate(); }
            if (num.signum() == 0) return ZERO;
            BigInteger g = num.gcd(den);
            if (g.equals(BigInteger.ONE)) return new Q(num, den);
            return new Q(num.divide(g), den.divide(g));
        }

        Q add(Q o) { return of(n.multiply(o.d).add(o.n.multiply(d)), d.multiply(o.d)); }
        Q sub(Q o) { return of(n.multiply(o.d).subtract(o.n.multiply(d)), d.multiply(o.d)); }
        Q mul(Q o) { return of(n.multiply(o.n), d.multiply(o.d)); }

        Q div(Q o) {
            if (o.n.signum() == 0) throw new CalcError("division by zero");
            return of(n.multiply(o.d), d.multiply(o.n));
        }

        Q neg() { return new Q(n.negate(), d); }

        Q abs() { return n.signum() < 0 ? new Q(n.negate(), d) : this; }

        int signum() { return n.signum(); }

        int compareTo(Q o) { return n.multiply(o.d).compareTo(o.n.multiply(d)); }

        boolean isInteger() { return d.equals(BigInteger.ONE); }

        BigInteger floor() {
            BigInteger[] qr = n.divideAndRemainder(d);
            return qr[1].signum() < 0 ? qr[0].subtract(BigInteger.ONE) : qr[0];
        }

        BigInteger ceil() {
            BigInteger[] qr = n.divideAndRemainder(d);
            return qr[1].signum() > 0 ? qr[0].add(BigInteger.ONE) : qr[0];
        }

        /** Truncation towards zero. */
        BigInteger trunc() { return n.divide(d); }

        /** Requires this value to be an integer. */
        BigInteger toInteger(String what) {
            if (!isInteger()) throw new CalcError(what + " requires an integer, got " + this);
            return n;
        }

        @Override public String toString() {
            return d.equals(BigInteger.ONE) ? n.toString() : n + "/" + d;
        }
    }

    // =================================================================
    //  tokenizer
    // =================================================================

    enum T { NUM, IDENT, OP, LPAREN, RPAREN, COMMA, END }

    static final class Tok {
        final T type;
        final String text;
        final BigInteger num;

        Tok(T type, String text, BigInteger num) {
            this.type = type;
            this.text = text;
            this.num = num;
        }
    }

    static List<Tok> tokenize(String src) {
        List<Tok> out = new ArrayList<>();
        int i = 0, len = src.length();
        while (i < len) {
            char c = src.charAt(i);
            if (Character.isWhitespace(c)) { i++; continue; }
            if (Character.isDigit(c)) {
                int start = i;
                if (c == '0' && i + 1 < len && (src.charAt(i + 1) == 'x' || src.charAt(i + 1) == 'X')) {
                    i += 2;
                    while (i < len && (isHex(src.charAt(i)) || src.charAt(i) == '_')) i++;
                    out.add(new Tok(T.NUM, src.substring(start, i),
                            new BigInteger(src.substring(start + 2, i).replace("_", ""), 16)));
                    continue;
                }
                if (c == '0' && i + 1 < len && (src.charAt(i + 1) == 'b' || src.charAt(i + 1) == 'B')) {
                    i += 2;
                    while (i < len && (src.charAt(i) == '0' || src.charAt(i) == '1' || src.charAt(i) == '_')) i++;
                    out.add(new Tok(T.NUM, src.substring(start, i),
                            new BigInteger(src.substring(start + 2, i).replace("_", ""), 2)));
                    continue;
                }
                if (c == '0' && i + 1 < len && (src.charAt(i + 1) == 'o' || src.charAt(i + 1) == 'O')) {
                    i += 2;
                    while (i < len && ((src.charAt(i) >= '0' && src.charAt(i) <= '7') || src.charAt(i) == '_')) i++;
                    out.add(new Tok(T.NUM, src.substring(start, i),
                            new BigInteger(src.substring(start + 2, i).replace("_", ""), 8)));
                    continue;
                }
                while (i < len && (Character.isDigit(src.charAt(i)) || src.charAt(i) == '_')) i++;
                out.add(new Tok(T.NUM, src.substring(start, i),
                        new BigInteger(src.substring(start, i).replace("_", ""), 10)));
                continue;
            }
            if (Character.isLetter(c) || c == '_') {
                int start = i;
                while (i < len && (Character.isLetterOrDigit(src.charAt(i)) || src.charAt(i) == '_')) i++;
                out.add(new Tok(T.IDENT, src.substring(start, i), null));
                continue;
            }
            switch (c) {
                case '(': out.add(new Tok(T.LPAREN, "(", null)); i++; continue;
                case ')': out.add(new Tok(T.RPAREN, ")", null)); i++; continue;
                case ',': out.add(new Tok(T.COMMA, ",", null)); i++; continue;
                case '+': case '-': case '*': case '/': case '%': case '^': case '!': case '=':
                    out.add(new Tok(T.OP, String.valueOf(c), null)); i++; continue;
                default:
                    throw new CalcError("unexpected character '" + c + "'");
            }
        }
        out.add(new Tok(T.END, "", null));
        return out;
    }

    static boolean isHex(char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    /** Formats a token verbatim; used only for diagnostics. */
    static String tokenText(Tok t) {
        return t.type == T.END ? "end of input" : t.text;
    }

    // =================================================================
    //  parser  (recursive descent, precedence climbing)
    // =================================================================

    static final class Parser {
        private final List<Tok> toks;
        private final Engine engine;
        private int pos;

        Parser(List<Tok> toks, Engine engine) {
            this.toks = toks;
            this.engine = engine;
        }

        private Tok peek() { return toks.get(pos); }

        private boolean acceptOp(String op) {
            Tok t = peek();
            if (t.type == T.OP && t.text.equals(op)) { pos++; return true; }
            return false;
        }

        private boolean accept(T type) {
            if (peek().type == type) { pos++; return true; }
            return false;
        }

        private void expect(T type, String what) {
            if (!accept(type)) throw new CalcError("expected " + what + ", found " + tokenText(peek()));
        }

        /** statement := IDENT '=' statement | additive */
        Q statement() {
            Tok t = peek();
            if (t.type == T.IDENT && pos + 1 < toks.size()
                    && toks.get(pos + 1).type == T.OP && toks.get(pos + 1).text.equals("=")) {
                String name = t.text;
                pos += 2;
                Q value = statement();
                engine.vars.put(name, value);
                return value;
            }
            return additive();
        }

        Q additive() {
            Q left = multiplicative();
            while (true) {
                if (acceptOp("+")) left = left.add(multiplicative());
                else if (acceptOp("-")) left = left.sub(multiplicative());
                else return left;
            }
        }

        Q multiplicative() {
            Q left = unary();
            while (true) {
                if (acceptOp("*")) left = left.mul(unary());
                else if (acceptOp("/")) left = left.div(unary());
                else if (acceptOp("%")) left = modulo(left, unary());
                else return left;
            }
        }

        /** a % b == a - b * floor(a / b)  (result always has b's sign convention: in [0, |b|)) */
        static Q modulo(Q a, Q b) {
            if (b.signum() == 0) throw new CalcError("division by zero");
            return a.sub(b.mul(Q.of(a.div(b).floor())));
        }

        Q unary() {
            if (acceptOp("-")) return unary().neg();
            if (acceptOp("+")) return unary();
            return power();
        }

        Q power() {
            Q base = postfix();
            if (acceptOp("^")) {
                Q exponent = unary(); // right associative, allows 2^-3 and 2^3^2
                return engine.power(base, exponent);
            }
            return base;
        }

        Q postfix() {
            Q value = atom();
            while (acceptOp("!")) value = Q.of(Engine.factorial(value.toInteger("factorial")));
            return value;
        }

        Q atom() {
            Tok t = peek();
            switch (t.type) {
                case NUM: pos++; return Q.of(t.num);
                case LPAREN: {
                    pos++;
                    Q v = statement();
                    expect(T.RPAREN, "')'");
                    return v;
                }
                case IDENT: {
                    pos++;
                    String name = t.text;
                    if (accept(T.LPAREN)) {
                        List<Q> args = new ArrayList<>();
                        if (!accept(T.RPAREN)) {
                            do { args.add(statement()); } while (accept(T.COMMA));
                            expect(T.RPAREN, "')'");
                        }
                        return engine.call(name, args);
                    }
                    if (name.equals("ans")) return engine.last;
                    Q v = engine.vars.get(name);
                    if (v == null) throw new CalcError("undefined variable '" + name + "'");
                    return v;
                }
                default:
                    throw new CalcError("unexpected " + tokenText(t));
            }
        }
    }

    // =================================================================
    //  engine
    // =================================================================

    static final class Engine {
        final Map<String, Q> vars = new LinkedHashMap<>();
        Q last = Q.ZERO;
        int digits = 30;      // decimal places shown for non-integers; 0 = exact only
        int base = 10;        // output base for integers
        boolean frac = false; // force n/d display for non-integers
        boolean timing = false;

        // ---------- integer helpers ----------

        static BigInteger factorial(BigInteger n) {
            if (n.signum() < 0) throw new CalcError("factorial requires a non-negative integer");
            if (n.bitLength() > 31) throw new CalcError("factorial argument is astronomically large");
            int k = n.intValue();
            return k < 2 ? BigInteger.ONE : productRange(2, k);
        }

        /** Binary splitting: multiply [lo, hi] as a balanced product tree. */
        static BigInteger productRange(int lo, int hi) {
            if (lo > hi) return BigInteger.ONE;
            if (lo == hi) return BigInteger.valueOf(lo);
            if (hi - lo == 1) return BigInteger.valueOf(lo).multiply(BigInteger.valueOf(hi));
            int mid = (lo + hi) >>> 1;
            return productRange(lo, mid).multiply(productRange(mid + 1, hi));
        }

        static BigInteger fibonacci(BigInteger n) {
            if (n.signum() < 0) throw new CalcError("fib requires a non-negative integer");
            if (n.bitLength() > 31) throw new CalcError("fib argument is too large");
            int k = n.intValue();
            BigInteger a = BigInteger.ZERO, b = BigInteger.ONE;
            for (int i = 31 - Integer.numberOfLeadingZeros(Math.max(1, k)); i >= 0; i--) {
                BigInteger d = a.multiply(b.shiftLeft(1).subtract(a));
                BigInteger e = a.multiply(a).add(b.multiply(b));
                if (((k >>> i) & 1) == 1) { a = e; b = d.add(e); }
                else { a = d; b = e; }
            }
            return a;
        }

        /** floor(a^(1/k)) for a >= 0, k >= 1, via Newton iteration. */
        static BigInteger nthRootFloor(BigInteger a, int k) {
            if (k < 1) throw new CalcError("root index must be >= 1");
            if (a.signum() < 0) throw new CalcError("nthRootFloor needs a non-negative operand");
            if (a.signum() == 0) return BigInteger.ZERO;
            if (k == 1) return a;
            BigInteger x = BigInteger.ONE.shiftLeft((a.bitLength() + k - 1) / k);
            BigInteger kMinus1 = BigInteger.valueOf(k - 1);
            BigInteger kBig = BigInteger.valueOf(k);
            while (true) {
                BigInteger y = x.multiply(kMinus1).add(a.divide(x.pow(k - 1))).divide(kBig);
                if (y.compareTo(x) >= 0) break;
                x = y;
            }
            while (x.pow(k).compareTo(a) > 0) x = x.subtract(BigInteger.ONE);
            while (x.add(BigInteger.ONE).pow(k).compareTo(a) <= 0) x = x.add(BigInteger.ONE);
            return x;
        }

        /** Exact rational k-th root, or null when it is irrational. */
        static Q exactRoot(Q value, int k) {
            boolean negative = value.signum() < 0;
            if (negative && (k & 1) == 0) return null;
            BigInteger num = value.n.abs();
            BigInteger den = value.d;
            BigInteger rn = nthRootFloor(num, k);
            if (!rn.pow(k).equals(num)) return null;
            BigInteger rd = nthRootFloor(den, k);
            if (!rd.pow(k).equals(den)) return null;
            Q r = Q.of(rn, rd);
            return negative ? r.neg() : r;
        }

        Q power(Q base, Q exponent) {
            if (exponent.isInteger()) {
                BigInteger e = exponent.n;
                if (e.bitLength() > 31) throw new CalcError("exponent is too large to materialise");
                int k = e.intValue();
                if (k >= 0) {
                    if (base.isInteger()) return Q.of(base.n.pow(k));
                    return Q.of(base.n.pow(k), base.d.pow(k));
                }
                if (base.signum() == 0) throw new CalcError("division by zero (0 raised to a negative power)");
                int m = -k;
                if (base.isInteger()) return Q.of(BigInteger.ONE, base.n.pow(m));
                return Q.of(base.d.pow(m), base.n.pow(m));
            }
            // rational exponent p/q -> try an exact q-th root first
            BigInteger p = exponent.n;
            BigInteger q = exponent.d;
            if (q.bitLength() > 20) throw new CalcError("root index is too large");
            int k = q.intValue();
            Q root = exactRoot(base, k);
            if (root == null) {
                throw new CalcError("result is irrational; use pow(a, b, digits) or sqrt(x, digits)");
            }
            if (p.bitLength() > 31) throw new CalcError("exponent is too large to materialise");
            int pi = p.intValue();
            if (pi >= 0) {
                return base.isInteger() ? Q.of(root.n.pow(pi)) : Q.of(root.n.pow(pi), root.d.pow(pi));
            }
            if (root.signum() == 0) throw new CalcError("division by zero");
            int pm = -pi;
            return Q.of(root.d.pow(pm), root.n.pow(pm));
        }

        // ---------- decimal rendering ----------

        /** True when the denominator is 2^a * 5^b, i.e. the decimal expansion terminates. */
        static boolean terminates(Q v) {
            BigInteger t = v.d;
            while (!t.testBit(0)) t = t.shiftRight(1);
            BigInteger five = BigInteger.valueOf(5);
            while (t.mod(five).signum() == 0) t = t.divide(five);
            return t.equals(BigInteger.ONE);
        }

        /** Exact (not rounded) decimal expansion of a rational with a terminating expansion. */
        static String exactDecimal(Q v) {
            boolean negative = v.signum() < 0;
            BigInteger num = v.n.abs();
            BigInteger den = v.d;
            int twos = 0, fives = 0;
            BigInteger t = den;
            while (!t.testBit(0)) { t = t.shiftRight(1); twos++; }
            BigInteger five = BigInteger.valueOf(5);
            while (t.mod(five).signum() == 0) { t = t.divide(five); fives++; }
            int scale = Math.max(twos, fives);
            String digits = num.multiply(BigInteger.TEN.pow(scale)).divide(den).toString();
            if (digits.length() <= scale) {
                StringBuilder pad = new StringBuilder();
                for (int i = digits.length(); i < scale; i++) pad.append('0');
                digits = pad + digits;
            }
            int cut = digits.length() - scale;
            String head = cut == 0 ? "0" : digits.substring(0, cut);
            String tail = digits.substring(cut);
            int end = tail.length();
            while (end > 0 && tail.charAt(end - 1) == '0') end--;
            tail = tail.substring(0, end);
            return (negative ? "-" : "") + head + (tail.isEmpty() ? "" : "." + tail);
        }

        /** Decimal string of n/d with {@code scale} fractional digits, half-up. */
        static String toDecimal(Q v, int scale) {
            boolean negative = v.signum() < 0;
            BigInteger num = v.n.abs();
            BigInteger den = v.d;
            BigInteger pow = BigInteger.TEN.pow(scale);
            BigInteger[] qr = num.multiply(pow).divideAndRemainder(den);
            BigInteger scaled = qr[0];
            if (qr[1].shiftLeft(1).compareTo(den) >= 0) scaled = scaled.add(BigInteger.ONE);

            String digits = scaled.toString();
            if (scale == 0) return (negative ? "-" : "") + digits;
            if (digits.length() <= scale) {
                StringBuilder sb = new StringBuilder();
                sb.append(negative ? "-" : "");
                sb.append("0.");
                for (int i = digits.length(); i < scale; i++) sb.append('0');
                sb.append(digits);
                return sb.toString();
            }
            int cut = digits.length() - scale;
            return (negative ? "-" : "") + digits.substring(0, cut) + "." + digits.substring(cut);
        }

        // ---------- pi / e ----------

        /** pi to {@code digits} decimals via Chudnovsky + binary splitting (exact integer arithmetic). */
        static BigDecimal piDecimal(int digits) {
            int terms = (int) (digits / 14.181647462725477) + 2;
            MathContext mc = new MathContext(digits + 10, RoundingMode.HALF_EVEN);
            BigInteger[] pqt = chudnovsky(0, terms);
            BigInteger q = pqt[1], t = pqt[2];
            // pi = (426880 * sqrt(10005) * q) / t
            BigDecimal sqrt10005 = BigDecimal.valueOf(10005).sqrt(mc);
            BigDecimal numerator = new BigDecimal(q).multiply(BigDecimal.valueOf(426880L)).multiply(sqrt10005, mc);
            BigDecimal pi = numerator.divide(new BigDecimal(t), mc);
            return pi.setScale(digits, RoundingMode.HALF_EVEN);
        }

        private static final BigInteger C3_OVER_24 =
                BigInteger.valueOf(640320).pow(3).divide(BigInteger.valueOf(24));

        /** Returns {P, Q, T} for the Chudnovsky series over [a, b). */
        private static BigInteger[] chudnovsky(int a, int b) {
            if (b - a == 1) {
                BigInteger pab, qab;
                if (a == 0) {
                    pab = BigInteger.ONE;
                    qab = BigInteger.ONE;
                } else {
                    BigInteger ai = BigInteger.valueOf(a);
                    BigInteger six = BigInteger.valueOf(6);
                    pab = ai.multiply(six).subtract(BigInteger.valueOf(5))
                            .multiply(ai.multiply(BigInteger.TWO).subtract(BigInteger.ONE))
                            .multiply(ai.multiply(six).subtract(BigInteger.ONE));
                    qab = ai.pow(3).multiply(C3_OVER_24);
                }
                BigInteger tab = pab.multiply(BigInteger.valueOf(13591409L)
                        .add(BigInteger.valueOf(545140134L).multiply(BigInteger.valueOf(a))));
                if ((a & 1) == 1) tab = tab.negate();
                return new BigInteger[]{pab, qab, tab};
            }
            int m = (a + b) >>> 1;
            BigInteger[] left = chudnovsky(a, m);
            BigInteger[] right = chudnovsky(m, b);
            return new BigInteger[]{
                    left[0].multiply(right[0]),
                    left[1].multiply(right[1]),
                    right[1].multiply(left[2]).add(left[0].multiply(right[2]))
            };
        }

        /** e to {@code digits} decimals via the 1/k! series. */
        static BigDecimal eDecimal(int digits) {
            MathContext mc = new MathContext(digits + 10, RoundingMode.HALF_EVEN);
            BigDecimal sum = BigDecimal.ONE;
            BigDecimal term = BigDecimal.ONE;
            BigDecimal limit = BigDecimal.ONE.movePointLeft(digits + 5);
            for (int k = 1; k < 1_000_000; k++) {
                term = term.divide(BigDecimal.valueOf(k), mc);
                if (term.abs().compareTo(limit) < 0) break;
                sum = sum.add(term, mc);
            }
            return sum.setScale(digits, RoundingMode.HALF_EVEN);
        }

        static Q fromDecimal(BigDecimal v) {
            int scale = v.scale();
            if (scale >= 0) return Q.of(v.unscaledValue(), BigInteger.TEN.pow(scale));
            return Q.of(v.unscaledValue().multiply(BigInteger.TEN.pow(-scale)));
        }

        // ---------- function dispatch ----------

        private static void arity(String name, List<Q> a, int exact) {
            if (a.size() != exact) {
                throw new CalcError(name + "() takes " + exact + " argument(s), got " + a.size());
            }
        }

        private static void minArity(String name, List<Q> a, int min) {
            if (a.size() < min) {
                throw new CalcError(name + "() takes at least " + min + " argument(s), got " + a.size());
            }
        }

        private static int smallIndex(Q v, String what) {
            BigInteger i = v.toInteger(what);
            if (i.bitLength() > 20) throw new CalcError(what + " is too large");
            return i.intValue();
        }

        Q call(String name, List<Q> a) {
            switch (name) {
                // ---- exact, always available ----
                case "abs":   arity(name, a, 1); return a.get(0).abs();
                case "floor": arity(name, a, 1); return Q.of(a.get(0).floor());
                case "ceil":  arity(name, a, 1); return Q.of(a.get(0).ceil());
                case "trunc": arity(name, a, 1); return Q.of(a.get(0).trunc());
                case "round": arity(name, a, 1); return Q.of(a.get(0).add(Q.of(BigInteger.ONE, BigInteger.TWO)).floor());
                case "sign":  arity(name, a, 1); return Q.of(BigInteger.valueOf(a.get(0).signum()));

                case "gcd": {
                    arity(name, a, 2);
                    return Q.of(a.get(0).toInteger("gcd").gcd(a.get(1).toInteger("gcd")));
                }
                case "lcm": {
                    arity(name, a, 2);
                    BigInteger x = a.get(0).toInteger("lcm"), y = a.get(1).toInteger("lcm");
                    if (x.signum() == 0 || y.signum() == 0) return Q.ZERO;
                    return Q.of(x.divide(x.gcd(y)).multiply(y).abs());
                }
                case "min": {
                    minArity(name, a, 1);
                    Q r = a.get(0);
                    for (Q v : a) if (v.compareTo(r) < 0) r = v;
                    return r;
                }
                case "max": {
                    minArity(name, a, 1);
                    Q r = a.get(0);
                    for (Q v : a) if (v.compareTo(r) > 0) r = v;
                    return r;
                }

                case "fact": arity(name, a, 1); return Q.of(factorial(a.get(0).toInteger("fact")));
                case "fib":  arity(name, a, 1); return Q.of(fibonacci(a.get(0).toInteger("fib")));

                case "isqrt": {
                    arity(name, a, 1);
                    BigInteger v = a.get(0).toInteger("isqrt");
                    if (v.signum() < 0) throw new CalcError("isqrt requires a non-negative integer");
                    return Q.of(v.sqrt());
                }
                case "ndigits": {
                    arity(name, a, 1);
                    BigInteger v = a.get(0).toInteger("ndigits").abs();
                    return Q.of(BigInteger.valueOf(v.signum() == 0 ? 1 : v.toString().length()));
                }
                case "isprime": {
                    arity(name, a, 1);
                    BigInteger v = a.get(0).toInteger("isprime");
                    return v.signum() > 0 && v.isProbablePrime(100) ? Q.ONE : Q.ZERO;
                }
                case "nextprime": {
                    arity(name, a, 1);
                    BigInteger v = a.get(0).toInteger("nextprime");
                    if (v.compareTo(BigInteger.TWO) < 0) return Q.of(BigInteger.TWO);
                    BigInteger c = v.add(BigInteger.ONE);
                    while (!c.isProbablePrime(100)) c = c.add(BigInteger.ONE);
                    return Q.of(c);
                }

                case "pow": {
                    arity(name, a, 2);
                    return power(a.get(0), a.get(1));
                }

                // ---- exact roots: fail loudly when the result is irrational ----
                case "sqrt": case "cbrt": {
                    int k = name.equals("sqrt") ? 2 : 3;
                    if (a.size() == 1) {
                        Q r = exactRoot(a.get(0), k);
                        if (r == null) {
                            throw new CalcError(name + "(x) is irrational here; use " + name + "(x, digits)");
                        }
                        return r;
                    }
                    arity(name, a, 2);
                    return rootApprox(a.get(0), k, smallIndex(a.get(1), "digits"));
                }
                case "root": {
                    if (a.size() == 2) {
                        int k = smallIndex(a.get(1), "root index");
                        Q r = exactRoot(a.get(0), k);
                        if (r == null) {
                            throw new CalcError("root(x, k) is irrational here; use root(x, k, digits)");
                        }
                        return r;
                    }
                    arity(name, a, 3);
                    return rootApprox(a.get(0), smallIndex(a.get(1), "root index"),
                            smallIndex(a.get(2), "digits"));
                }

                // ---- irrationals at a caller-chosen precision ----
                case "pi": {
                    arity(name, a, 1);
                    return fromDecimal(piDecimal(smallIndex(a.get(0), "digits")));
                }
                case "e": {
                    arity(name, a, 1);
                    return fromDecimal(eDecimal(smallIndex(a.get(0), "digits")));
                }

                default:
                    throw new CalcError("unknown function '" + name + "'");
            }
        }

        /** floor(|x|^(1/k) * 10^digits) with the sign restored, as an exact rational. */
        static Q rootApprox(Q value, int k, int digits) {
            if (k < 1) throw new CalcError("root index must be >= 1");
            if (digits < 0) throw new CalcError("digits must be >= 0");
            boolean negative = value.signum() < 0;
            if (negative && (k & 1) == 0) throw new CalcError("even root of a negative number is not real");
            BigInteger num = value.n.abs();
            BigInteger den = value.d;
            BigInteger scale = BigInteger.TEN.pow(digits);
            // floor( (num/den)^(1/k) * scale ) == floor( (num * scale^k / den)^(1/k) )
            BigInteger inner = num.multiply(scale.pow(k)).divide(den);
            BigInteger root = nthRootFloor(inner, k);
            Q r = Q.of(root, scale);
            return negative ? r.neg() : r;
        }

        Q evaluate(String line) {
            Parser p = new Parser(tokenize(line), this);
            Q v = p.statement();
            if (p.peek().type != T.END) {
                throw new CalcError("unexpected trailing input: " + tokenText(p.peek()));
            }
            return v;
        }
    }

    // =================================================================
    //  REPL
    // =================================================================

    static final String HELP = String.join("\n",
            "BigCalc 1.0 - exact arbitrary-precision calculator (no digit limit)",
            "",
            "Operators     + - * / % ^ !   and parentheses; ^ is right associative",
            "               % is a - b*floor(a/b); ! is postfix factorial",
            "Literals      decimal, 0x.. hex, 0b.. binary, 0o.. octal, _ separators",
            "Variables     x = <expr>   (also: ans = previous result)",
            "Exact funcs   abs floor ceil trunc round sign gcd lcm min max",
            "              fact fib isqrt ndigits isprime nextprime pow root",
            "Precision     sqrt(x, d)  root(x, k, d)  pi(d)  e(d)",
            "              (sqrt/root without d fail unless the result is exact)",
            "",
            "Commands",
            "  :digits N   decimals shown for non-integers (default 30, 0 = exact only)",
            "  :base N     integer output base: 10, 16, 8 or 2",
            "  :hex :dec :bin :oct",
            "  :vars       list variables",
            "  :frac       force n/d display for non-integers (auto shows an exact",
            "              decimal when the expansion terminates)",
            "  :time       toggle evaluation timing",
            "  :help       this text",
            "  :quit       exit",
            "",
            "Every value is an exact rational: 1/3 stays 1/3, 2^1000000 is exact,",
            "100000! is exact, and results are never truncated.");

    static void banner(PrintStream out) {
        out.println("BigCalc 1.0  -  exact arbitrary-precision calculator");
        out.println("type :help for help, :quit to exit");
    }

    static void printValue(PrintStream out, Engine engine, Q v) {
        if (v.isInteger()) {
            out.println(v.n.toString(engine.base));
            return;
        }
        if (!engine.frac && engine.base == 10 && Engine.terminates(v)) {
            out.println(Engine.exactDecimal(v));
            return;
        }
        out.println(v.toString());
        if (engine.digits > 0 && engine.base == 10) {
            out.println("  = " + Engine.toDecimal(v, engine.digits)
                    + "  (rounded to " + engine.digits + " decimals)");
        }
    }

    public static void main(String[] args) throws IOException {
        PrintStream out = new PrintStream(
                new BufferedOutputStream(new FileOutputStream(FileDescriptor.out), 1 << 20),
                false, StandardCharsets.UTF_8);
        BufferedReader in = new BufferedReader(
                new InputStreamReader(System.in, StandardCharsets.UTF_8), 1 << 16);

        Engine engine = new Engine();
        boolean interactive = System.console() != null;

        if (interactive) banner(out);

        String line;
        while (true) {
            if (interactive) { out.print("> "); out.flush(); }
            line = in.readLine();
            if (line == null) break;
            line = line.trim();
            if (line.isEmpty()) continue;

            if (line.charAt(0) == ':') {
                String[] parts = line.substring(1).trim().split("\\s+");
                String cmd = parts[0].toLowerCase();
                try {
                    switch (cmd) {
                        case "q": case "quit": case "exit":
                            out.flush();
                            return;
                        case "h": case "help":
                            out.println(HELP);
                            break;
                        case "d": case "digits": {
                            int d = Integer.parseInt(parts[1]);
                            if (d < 0) throw new CalcError("digits must be >= 0");
                            if (d > 10_000_000) throw new CalcError("digits is unreasonably large");
                            engine.digits = d;
                            out.println("digits = " + d);
                            break;
                        }
                        case "base": {
                            int b = Integer.parseInt(parts[1]);
                            if (b != 2 && b != 8 && b != 10 && b != 16) {
                                throw new CalcError("base must be 2, 8, 10 or 16");
                            }
                            engine.base = b;
                            out.println("base = " + b);
                            break;
                        }
                        case "hex": engine.base = 16; out.println("base = 16"); break;
                        case "dec": engine.base = 10; out.println("base = 10"); break;
                        case "oct": engine.base = 8;  out.println("base = 8"); break;
                        case "bin": engine.base = 2;  out.println("base = 2"); break;
                        case "vars":
                            if (engine.vars.isEmpty()) out.println("(no variables)");
                            else for (Map.Entry<String, Q> en : engine.vars.entrySet()) {
                                out.println("  " + en.getKey() + " = " + en.getValue());
                            }
                            break;
                        case "time":
                            engine.timing = !engine.timing;
                            out.println("timing " + (engine.timing ? "on" : "off"));
                            break;
                        case "frac":
                            engine.frac = parts.length > 1
                                    ? parts[1].equalsIgnoreCase("on") || parts[1].equals("1")
                                    : !engine.frac;
                            out.println("frac " + (engine.frac ? "on (exact fraction)" : "off (auto)"));
                            break;
                        default:
                            throw new CalcError("unknown command ':" + cmd + "' (try :help)");
                    }
                } catch (CalcError ex) {
                    out.println("error: " + ex.getMessage());
                } catch (ArrayIndexOutOfBoundsException ex) {
                    out.println("error: command :" + cmd + " needs an argument");
                } catch (NumberFormatException ex) {
                    out.println("error: command :" + cmd + " expects a number");
                }
                out.flush();
                continue;
            }

            long start = System.nanoTime();
            try {
                Q v = engine.evaluate(line);
                engine.last = v;
                printValue(out, engine, v);
                if (engine.timing) {
                    out.printf("  [%.1f ms]%n", (System.nanoTime() - start) / 1e6);
                }
            } catch (CalcError ex) {
                out.println("error: " + ex.getMessage());
            } catch (ArithmeticException ex) {
                out.println("error: " + ex.getMessage());
            }
            out.flush();
        }
        out.flush();
    }
}

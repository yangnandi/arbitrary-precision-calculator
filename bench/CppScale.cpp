// Schoolbook (naive O(n^2)) big-integer multiplication, base 2^32.
// Stands in for what a from-scratch C++ bignum would look like without GMP.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using u32 = uint32_t;
using u64 = uint64_t;

static std::vector<u32> schoolbook(const std::vector<u32>& a, const std::vector<u32>& b) {
    std::vector<u64> acc(a.size() + b.size() + 2, 0);
    for (size_t i = 0; i < a.size(); ++i) {
        const u64 ai = a[i];
        u64 carry = 0;
        for (size_t j = 0; j < b.size(); ++j) {
            // ai*b[j] <= (2^32-1)^2, plus two < 2^32 terms -> still fits in u64
            u64 cur = acc[i + j] + ai * b[j] + carry;
            acc[i + j] = cur & 0xffffffffULL;
            carry = cur >> 32;
        }
        size_t k = i + b.size();
        while (carry) {
            u64 cur = acc[k] + carry;
            acc[k] = cur & 0xffffffffULL;
            carry = cur >> 32;
            ++k;
        }
    }
    std::vector<u32> out(acc.begin(), acc.end());
    while (out.size() > 1 && out.back() == 0) out.pop_back();
    return out;
}

int main() {
    const size_t digits[] = {50000, 100000, 200000};
    double times[3];
    for (int idx = 0; idx < 3; ++idx) {
        size_t d = digits[idx];
        size_t limbs = (size_t)(d * 3.3219280948873626 / 32.0) + 1;
        std::vector<u32> a(limbs), b(limbs);
        u64 s = 88172645463325252ULL;
        for (size_t i = 0; i < limbs; ++i) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; a[i] = (u32)s; }
        for (size_t i = 0; i < limbs; ++i) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; b[i] = (u32)s; }

        double best = 1e18;
        for (int r = 0; r < 3; ++r) {
            auto t0 = std::chrono::steady_clock::now();
            std::vector<u32> p = schoolbook(a, b);
            auto t1 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            if (ms < best) best = ms;
            if (p.empty()) std::printf("x");
        }
        times[idx] = best;
        std::printf("%9zu digits  %10.1f ms\n", d, best);
    }
    std::printf("\nfitted exponent between consecutive sizes:\n");
    for (int i = 1; i < 3; ++i) {
        double e = std::log(times[i] / times[i - 1]) / std::log((double)digits[i] / digits[i - 1]);
        std::printf("  %zu -> %zu : %.3f\n", digits[i - 1], digits[i], e);
    }
    std::printf("  overall %.3f\n",
                std::log(times[2] / times[0]) / std::log((double)digits[2] / digits[0]));
    return 0;
}

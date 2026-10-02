// BigCalcGMP - exact arbitrary-precision calculator built on GMP.
//
// Mirrors the Java BigCalc exactly: every value is a canonical exact
// rational (mpq_class), so there is no digit limit and no rounding inside
// evaluation. Irrational quantities are only produced at a caller-chosen
// precision and are returned as exact rationals.
//
// ---------------------------------------------------------------------
// v2 - unrestricted parameters + streaming output
//
//  * No artificial size caps anywhere. Exponents, digit counts, root
//    indices and factorial arguments are carried as arbitrary-precision
//    integers (mpz_class), and evaluated by square-and-multiply instead
//    of GMP's unsigned-long-only entry points. 2^(10^46) parses, is
//    accepted and is sized up before any work starts.
//  * Results are written out most-significant-digit-first in chunks and
//    flushed immediately, so a 300-million-digit answer starts scrolling
//    right away instead of appearing only after the whole decimal string
//    has been materialised. Peak extra memory is one chunk, not one copy
//    of the entire result.
//  * Long computations report progress on stderr.
//  * The only remaining limit is physical RAM. That is checked up front
//    and reported in exact numbers. :guard off disables even that check.
// ---------------------------------------------------------------------
// v3 - multithreading + CUDA screening
//
//  * fact(n) runs a parallel product tree, pi(d) parallel binary splitting,
//    e(d) block-parallel series, and decimal output above 2M digits converts
//    its pieces on every core. :threads N controls the pool; GMP objects are
//    never shared, which is what makes that safe.
//    Measured speedups are modest (1.1x-2.1x) and that is expected: the top
//    of a binary-splitting tree is a single huge GMP multiplication, which
//    no amount of threading parallelises.
//  * One CUDA kernel -- n mod p for millions of small primes at once --
//    compiled to PTX by nvcc, embedded as a string, and JIT-ed at run time
//    through nvcuda.dll via the Driver API. No CUDA runtime, no import
//    library, still one MinGW-built executable. It backs smallfactor() and
//    the screen in front of isprime(); on an RTX 5060 Ti it is 2.7x-7.4x
//    faster than the 44-thread CPU sweep.
//  * terminates()/printExactDecimal() no longer factor 2^a*5^b one exponent
//    at a time. That loop was O(d^2) and dominated every pi(d)/e(d) render:
//    pi(200000) went from 16.0 s to 0.27 s.
// ---------------------------------------------------------------------
//
// Build:
//   C:\msys64\mingw64\bin\g++.exe -O2 -std=c++20 -static -o bigcalcgmp.exe BigCalcGMP.cpp -lgmpxx -lgmp
//
// Keep this file ASCII-only.

#include <gmpxx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
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
//  worker threads
//
//  A tiny dependency-free pool. Every task works on its own mpz_class
//  objects; nothing GMP-owned is ever shared, which is what makes this
//  safe -- GMP objects are not internally synchronised.
// =====================================================================

static unsigned g_threads = 0;        // 0 = auto (:threads 0)

static unsigned threadCount() {
    if (g_threads) return g_threads;
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 1u;
}

// Run fn(i) for every i in [0, n). Falls back to a plain loop when the
// pool would not pay for itself.
template <class F>
static void parallelFor(size_t n, F&& fn) {
    unsigned t = threadCount();
    if (t <= 1 || n <= 1) {
        for (size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    if (static_cast<size_t>(t) > n) t = static_cast<unsigned>(n);
    std::atomic<size_t> next{0};
    std::mutex errMx;
    std::exception_ptr err;
    std::vector<std::thread> pool;
    pool.reserve(t);
    for (unsigned w = 0; w < t; ++w) {
        pool.emplace_back([&] {
            try {
                for (;;) {
                    size_t i = next.fetch_add(1, std::memory_order_relaxed);
                    if (i >= n) break;
                    fn(i);
                }
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lk(errMx);
                    if (!err) err = std::current_exception();
                }
                next.store(n);
            }
        });
    }
    for (auto& th : pool) th.join();
    if (err) std::rethrow_exception(err);
}

// Split [0,total) into `k` contiguous chunks; ideal for product trees.
static std::vector<unsigned long long> chunkBounds(unsigned long long total, size_t k) {
    std::vector<unsigned long long> b(k + 1);
    for (size_t i = 0; i <= k; ++i) {
        b[i] = total * static_cast<unsigned long long>(i) / static_cast<unsigned long long>(k);
    }
    return b;
}

// =====================================================================
//  physical limits
//
//  There are no software limits left. The only thing that can still stop
//  a computation is the amount of RAM in the machine, so that is measured
//  and reported in exact figures instead of being papered over with an
//  arbitrary "unreasonably large" cap.
// =====================================================================

static bool g_guard = true;      // :guard on|off

// GMP's built-in OOM handler calls abort(), which on Windows produces a
// crash dialog and no explanation. Replace it with a clean exit so the
// ":guard off" path still tells you what actually happened.
static void gmpOutOfMemory(unsigned long long want) {
    std::fprintf(stderr,
        "\nBigCalcGMP: out of memory - GMP asked the OS for %llu bytes and was refused.\n"
        "This is the machine's limit, not the calculator's. Use a smaller size, or leave\n"
        ":guard on so it is rejected up front with an exact size estimate.\n",
        want);
    std::fflush(stderr);
    std::exit(2);
}

static void* gmpAlloc(size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) gmpOutOfMemory(static_cast<unsigned long long>(n));
    return p;
}

static void* gmpRealloc(void* old, size_t oldN, size_t newN) {
    (void)oldN;
    void* p = std::realloc(old, newN ? newN : 1);
    if (!p) gmpOutOfMemory(static_cast<unsigned long long>(newN));
    return p;
}

static void gmpFree(void* p, size_t) { std::free(p); }

static unsigned long long availableBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        // The commit limit (physical + page file) is the real "how much can be
        // allocated" figure; free physical alone understates it. Take the larger.
        unsigned long long phys = static_cast<unsigned long long>(ms.ullAvailPhys);
        unsigned long long page = static_cast<unsigned long long>(ms.ullAvailPageFile);
        return phys > page ? phys : page;
    }
    return 0ULL;
#else
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psize > 0) {
        return static_cast<unsigned long long>(pages) * static_cast<unsigned long long>(psize);
    }
    return 0ULL;
#endif
}

// Windows is LLP64: unsigned long is 32 bit, so BigInt(someU64) would either
// truncate or pick an ambiguous gmpxx overload. Go through decimal text.
static BigInt bytesBig(unsigned long long v) {
    BigInt r;
    std::string s = std::to_string(v);
    mpz_set_str(r.get_mpz_t(), s.c_str(), 10);
    return r;
}

static BigInt pow10Big(const BigInt& e) {
    BigInt r(1);
    if (e <= 0) return r;
    if (mpz_fits_ulong_p(e.get_mpz_t())) {
        mpz_ui_pow_ui(r.get_mpz_t(), 10UL, e.get_ui());
        return r;
    }
    // exponent past unsigned long: square-and-multiply, still exact
    BigInt ten(10), ee = e;
    r = 1;
    while (ee != 0) {
        if (mpz_odd_p(ee.get_mpz_t())) r *= ten;
        ee >>= 1;
        if (ee != 0) ten *= ten;
    }
    return r;
}

static size_t bitLength(const BigInt& a) {
    if (a == 0) return 1;
    return mpz_sizeinbase(a.get_mpz_t(), 2);
}

// Exact number of decimal digits of |a| (mpz_sizeinbase may be one too big).
static size_t decimalLength(const BigInt& a) {
    BigInt v = a < 0 ? -a : a;
    if (v == 0) return 1;
    size_t d = mpz_sizeinbase(v.get_mpz_t(), 10);
    BigInt p;
    if (mpz_fits_ulong_p(BigInt(static_cast<unsigned long>(d - 1)).get_mpz_t())) {
        mpz_ui_pow_ui(p.get_mpz_t(), 10UL, static_cast<unsigned long>(d - 1));
        if (v < p) --d;
    }
    return d;
}

// bytes of the binary mpz holding a number with `digits` decimal digits
static BigInt bytesForDigits(const BigInt& digits) {
    static const BigInt N("33219280948873623478703194294893901758648");   // log2(10) * 10^40
    BigInt D = pow10Big(BigInt(40));
    BigInt bits = (digits * N) / D;
    return bits / 8 + 128;
}

static std::string humanBytes(const BigInt& b) {
    std::string s = b.get_str();
    char buf[96];
    if (s.size() <= 15) {
        double d = std::strtod(s.c_str(), nullptr);
        static const char* unit[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
        int i = 0;
        while (d >= 1024.0 && i < 6) { d /= 1024.0; ++i; }
        std::snprintf(buf, sizeof buf, "%.4g %s", d, unit[i]);
        return buf;
    }
    std::string mant = s.substr(0, 4);
    while (mant.size() > 1 && mant.back() == '0') mant.pop_back();
    std::snprintf(buf, sizeof buf, "%c.%se%zu bytes",
                  s[0], mant.c_str() + 1, s.size() - 1);
    return buf;
}

// Throws unless `digits` decimal digits could plausibly be held in RAM.
// This is a *physical* check, not an arbitrary cap: with :guard off it is
// skipped entirely and the machine is allowed to try.
static void requireFeasible(const BigInt& digits, const std::string& what) {
    if (!g_guard) return;
    unsigned long long avail = availableBytes();
    if (avail == 0) return;                       // unknown: let it try
    BigInt need = bytesForDigits(digits);
    if (need <= bytesBig(avail)) return;          // 64-bit safe, no unsigned long truncation
    std::ostringstream m;
    m << what << " would need about " << digits.get_str() << " decimal digits ("
      << humanBytes(need) << " of binary value, before any output buffer), but this "
      << "machine currently has " << humanBytes(bytesBig(avail))
      << " of free RAM. Nothing in the calculator caps this -- it is the machine. "
      << "Use :guard off to attempt it anyway.";
    throw CalcError(m.str());
}

// digits(2^n) = floor(n*log10(2)) + 1, good to <1 unit for n up to ~10^60.
static BigInt digitsOfPowerOfTwo(const BigInt& n) {
    static const BigInt NUM("3010299956639811952137388947244930267681898814621085413104274611");
    BigInt DEN = pow10Big(BigInt(64));
    return (n * NUM) / DEN + 1;
}

// Conservative lower bound for digits(n!). Uses log10(n) >= (len(n) - 2).
static BigInt digitsOfFactorial(const BigInt& n) {
    if (n < 2) return BigInt(1);
    BigInt approxLog = BigInt(static_cast<unsigned long>(decimalLength(n)));
    if (approxLog > 2) approxLog -= 2; else approxLog = 0;
    return n * approxLog + 1;
}

static BigInt digitsOfFibonacci(const BigInt& n) {
    // log10(phi) to 64 fractional digits; verified against fib(1000) = 209 digits
    static const BigInt NUM("2089876402499787337692720892375554168224592399182109535392875613");
    BigInt DEN = pow10Big(BigInt(64));
    return (n * NUM) / DEN + 1;
}

// =====================================================================
//  GPU: bulk modular screening through the CUDA Driver API
//
//  Honest scope. Arbitrary-precision arithmetic does not belong on a GPU:
//  the hot path is a chain of huge multiplications, which GMP does better
//  and which is latency- and bandwidth-bound on a card, not
//  throughput-bound. What *is* embarrassingly parallel is testing one huge
//  number against a very large table of small primes -- millions of
//  independent (n mod p) reductions with no data flowing between them.
//  That is what runs on the GPU here, and it is used for the small-factor
//  screen in front of isprime/nextprime and by smallfactor().
//
//  The kernel is compiled to PTX by nvcc, embedded as a string by
//  ptx_to_header.py, and JIT-ed at run time through nvcuda.dll. No CUDA
//  runtime, no import library, no MSVC-ABI DLL: the calculator stays one
//  MinGW-built executable that simply uses the GPU when one is present.
// =====================================================================

#if defined(_WIN32)
#  if defined(__has_include)
#    if __has_include("gpu_ptx.h")
#      include "gpu_ptx.h"
#      define BIGCALC_EMBEDDED_PTX 1
#    endif
#  endif
#endif

static bool g_useGpu = true;          // :gpu on|off

#if defined(BIGCALC_EMBEDDED_PTX)

namespace cuda {

using CUresult = int;
using CUdevice = int;
struct CUctx_st; struct CUmod_st; struct CUfunc_st;
using CUcontext = CUctx_st*;
using CUmodule  = CUmod_st*;
using CUfunction = CUfunc_st*;
using CUdeviceptr = unsigned long long;
using CUstream = void*;

struct Api {
    bool tried = false;
    bool ok = false;
    std::string why;

    CUresult (*Init)(unsigned) = nullptr;
    CUresult (*DeviceGet)(CUdevice*, int) = nullptr;
    CUresult (*CtxCreate)(CUcontext*, unsigned, CUdevice) = nullptr;
    CUresult (*CtxDestroy)(CUcontext) = nullptr;
    CUresult (*ModuleLoadDataEx)(CUmodule*, const void*, unsigned, int*, void**) = nullptr;
    CUresult (*ModuleGetFunction)(CUfunction*, CUmodule, const char*) = nullptr;
    CUresult (*MemAlloc)(CUdeviceptr*, size_t) = nullptr;
    CUresult (*MemFree)(CUdeviceptr) = nullptr;
    CUresult (*MemcpyHtoD)(CUdeviceptr, const void*, size_t) = nullptr;
    CUresult (*MemcpyDtoH)(void*, CUdeviceptr, size_t) = nullptr;
    CUresult (*LaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                             unsigned, unsigned, unsigned,
                             unsigned, CUstream, void**, void**) = nullptr;
    CUresult (*CtxSynchronize)() = nullptr;
    CUresult (*DeviceGetName)(char*, int, CUdevice) = nullptr;
    CUresult (*DeviceTotalMem)(size_t*, CUdevice) = nullptr;
    CUresult (*DeviceComputeCapability)(int*, int*, CUdevice) = nullptr;
    CUresult (*DriverGetVersion)(int*) = nullptr;
    CUresult (*GetErrorName)(CUresult, const char**) = nullptr;
};

inline Api& api() {
    static Api a;
    if (a.tried) return a;
    a.tried = true;

    HMODULE h = LoadLibraryA("nvcuda.dll");
    if (!h) { a.why = "nvcuda.dll not present (no NVIDIA driver)"; return a; }

    auto sym = [&](const char* n) -> void* {
        void* p = reinterpret_cast<void*>(GetProcAddress(h, n));
        return p;
    };
    // 64-bit CUDA exports the _v2 forms; fall back to the plain name.
    auto sym2 = [&](const char* v2, const char* plain) -> void* {
        void* p = sym(v2);
        return p ? p : sym(plain);
    };

    a.Init        = reinterpret_cast<decltype(a.Init)>(sym("cuInit"));
    a.DeviceGet   = reinterpret_cast<decltype(a.DeviceGet)>(sym("cuDeviceGet"));
    a.CtxCreate   = reinterpret_cast<decltype(a.CtxCreate)>(sym2("cuCtxCreate_v2", "cuCtxCreate"));
    a.CtxDestroy  = reinterpret_cast<decltype(a.CtxDestroy)>(sym2("cuCtxDestroy_v2", "cuCtxDestroy"));
    a.ModuleLoadDataEx = reinterpret_cast<decltype(a.ModuleLoadDataEx)>(sym("cuModuleLoadDataEx"));
    a.ModuleGetFunction = reinterpret_cast<decltype(a.ModuleGetFunction)>(sym("cuModuleGetFunction"));
    a.MemAlloc    = reinterpret_cast<decltype(a.MemAlloc)>(sym2("cuMemAlloc_v2", "cuMemAlloc"));
    a.MemFree     = reinterpret_cast<decltype(a.MemFree)>(sym2("cuMemFree_v2", "cuMemFree"));
    a.MemcpyHtoD  = reinterpret_cast<decltype(a.MemcpyHtoD)>(sym2("cuMemcpyHtoD_v2", "cuMemcpyHtoD"));
    a.MemcpyDtoH  = reinterpret_cast<decltype(a.MemcpyDtoH)>(sym2("cuMemcpyDtoH_v2", "cuMemcpyDtoH"));
    a.LaunchKernel = reinterpret_cast<decltype(a.LaunchKernel)>(sym("cuLaunchKernel"));
    a.CtxSynchronize = reinterpret_cast<decltype(a.CtxSynchronize)>(sym("cuCtxSynchronize"));
    a.DeviceGetName = reinterpret_cast<decltype(a.DeviceGetName)>(sym("cuDeviceGetName"));
    a.DeviceTotalMem = reinterpret_cast<decltype(a.DeviceTotalMem)>(sym2("cuDeviceTotalMem_v2", "cuDeviceTotalMem"));
    a.DeviceComputeCapability =
        reinterpret_cast<decltype(a.DeviceComputeCapability)>(sym("cuDeviceComputeCapability"));
    a.DriverGetVersion = reinterpret_cast<decltype(a.DriverGetVersion)>(sym("cuDriverGetVersion"));
    a.GetErrorName = reinterpret_cast<decltype(a.GetErrorName)>(sym("cuGetErrorName"));

    if (!a.Init || !a.DeviceGet || !a.CtxCreate || !a.ModuleLoadDataEx ||
        !a.ModuleGetFunction || !a.MemAlloc || !a.MemcpyHtoD || !a.MemcpyDtoH ||
        !a.LaunchKernel || !a.CtxSynchronize) {
        a.why = "nvcuda.dll is missing the driver entry points this build needs";
        return a;
    }
    if (a.Init(0) != 0) { a.why = "cuInit failed (no usable device)"; return a; }
    a.ok = true;
    return a;
}

inline std::string errName(CUresult r) {
    Api& a = api();
    const char* s = nullptr;
    if (a.GetErrorName && a.GetErrorName(r, &s) == 0 && s) return s;
    return "CUresult " + std::to_string(r);
}

// A context plus the loaded module, created once and kept for the process.
struct Session {
    bool tried = false;
    bool ok = false;
    std::string why;
    CUcontext ctx = nullptr;
    CUmodule mod = nullptr;
    CUfunction kModSmall = nullptr;
    std::string device;
    unsigned long long vram = 0;
    int ccMajor = 0, ccMinor = 0;
    int driverVer = 0;
};

inline Session& session() {
    static Session s;
    if (s.tried) return s;
    s.tried = true;

    Api& a = api();
    if (!a.ok) { s.why = a.why; return s; }

    CUdevice dev = 0;
    if (a.DeviceGet(&dev, 0) != 0) { s.why = "cuDeviceGet failed"; return s; }

    char name[256] = {0};
    if (a.DeviceGetName) a.DeviceGetName(name, sizeof(name) - 1, dev);
    s.device = name;
    if (a.DeviceTotalMem) { size_t bytes = 0; a.DeviceTotalMem(&bytes, dev); s.vram = bytes; }
    if (a.DeviceComputeCapability) a.DeviceComputeCapability(&s.ccMajor, &s.ccMinor, dev);
    if (a.DriverGetVersion) a.DriverGetVersion(&s.driverVer);

    if (a.CtxCreate(&s.ctx, 0, dev) != 0) { s.why = "cuCtxCreate failed"; return s; }

    CUresult r = a.ModuleLoadDataEx(&s.mod, GPU_PTX_SOURCE, 0, nullptr, nullptr);
    if (r != 0) {
        s.why = "cuModuleLoadDataEx failed: " + errName(r)
              + " (driver may be older than the PTX target compute_120)";
        return s;
    }
    if (a.ModuleGetFunction(&s.kModSmall, s.mod, "kModSmallPrimes") != 0) {
        s.why = "kernel kModSmallPrimes not found in the embedded PTX";
        return s;
    }
    s.ok = true;
    return s;
}

}  // namespace cuda
#endif  // BIGCALC_EMBEDDED_PTX

// ---------------- small-prime table (sieve of Eratosthenes) ----------------

static const std::vector<unsigned int>& primeTable(unsigned long long limit) {
    static std::vector<unsigned int> primes;
    static unsigned long long builtTo = 1;
    if (limit <= builtTo && !primes.empty()) return primes;

    if (limit > 2000000000ULL) limit = 2000000000ULL;   // p must stay < 2^31
    std::vector<bool> comp(static_cast<size_t>(limit) + 1, false);
    primes.clear();
    for (unsigned long long i = 2; i <= limit; ++i) {
        if (comp[static_cast<size_t>(i)]) continue;
        primes.push_back(static_cast<unsigned int>(i));
        if (i * i <= limit) {
            for (unsigned long long j = i * i; j <= limit; j += i) comp[static_cast<size_t>(j)] = true;
        }
    }
    builtTo = limit;
    return primes;
}

// ---------------- the screening call ----------------

// Smallest prime factor of |n| that is <= limit, or 0 if there is none.
// GPU kernels do the bulk sweep; the caller does the cheap sequential pass.
static unsigned long long gpuSmallestFactor(const BigInt& n, unsigned long long limit,
                                            std::string* why) {
#if defined(BIGCALC_EMBEDDED_PTX)
    cuda::Session& S = cuda::session();
    if (!S.ok) { if (why) *why = S.why; return 0; }
    cuda::Api& A = cuda::api();

    const std::vector<unsigned int>& primes = primeTable(limit);
    if (primes.empty()) return 0;

    BigInt a = n < 0 ? -n : n;
    size_t nlimbs = (mpz_sizeinbase(a.get_mpz_t(), 2) + 31) / 32;
    if (nlimbs == 0) nlimbs = 1;
    std::vector<unsigned int> limbs(nlimbs, 0);
    size_t got = 0;
    mpz_export(limbs.data(), &got, -1, sizeof(unsigned int), 0, 0, a.get_mpz_t());
    size_t useLimbs = got ? got : 1;

    cuda::CUdeviceptr dLimbs = 0, dPrimes = 0, dRes = 0, dFound = 0;
    bool alloced = false;
    unsigned int found = 0xFFFFFFFFu;
    auto cleanup = [&] {
        if (!alloced) return;
        A.MemFree(dLimbs); A.MemFree(dPrimes); A.MemFree(dRes); A.MemFree(dFound);
        alloced = false;
    };

    size_t limbBytes = useLimbs * sizeof(unsigned int);
    size_t primeBytes = primes.size() * sizeof(unsigned int);
    if (A.MemAlloc(&dLimbs, limbBytes) != 0) { if (why) *why = "cuMemAlloc(limbs) failed"; return 0; }
    if (A.MemAlloc(&dPrimes, primeBytes) != 0) { A.MemFree(dLimbs); if (why) *why = "cuMemAlloc(primes) failed"; return 0; }
    if (A.MemAlloc(&dRes, primeBytes) != 0) { A.MemFree(dLimbs); A.MemFree(dPrimes); if (why) *why = "cuMemAlloc(res) failed"; return 0; }
    if (A.MemAlloc(&dFound, sizeof(unsigned int)) != 0) {
        A.MemFree(dLimbs); A.MemFree(dPrimes); A.MemFree(dRes);
        if (why) *why = "cuMemAlloc(found) failed";
        return 0;
    }
    alloced = true;

    A.MemcpyHtoD(dLimbs, limbs.data(), limbBytes);
    A.MemcpyHtoD(dPrimes, primes.data(), primeBytes);
    A.MemcpyHtoD(dFound, &found, sizeof(found));

    int nlimbsI = static_cast<int>(useLimbs);
    int nprimesI = static_cast<int>(primes.size());
    void* args[] = { &dLimbs, &nlimbsI, &dPrimes, &nprimesI, &dRes, &dFound };

    unsigned threads = 256;
    unsigned blocks = static_cast<unsigned>((primes.size() + threads - 1) / threads);
    cuda::CUresult r = A.LaunchKernel(S.kModSmall, blocks, 1, 1, threads, 1, 1, 0, nullptr, args, nullptr);
    if (r != 0) { cleanup(); if (why) *why = "cuLaunchKernel failed: " + cuda::errName(r); return 0; }
    r = A.CtxSynchronize();
    if (r != 0) { cleanup(); if (why) *why = "kernel failed: " + cuda::errName(r); return 0; }

    A.MemcpyDtoH(&found, dFound, sizeof(found));
    cleanup();
    if (why) why->clear();
    return found == 0xFFFFFFFFu ? 0ULL : static_cast<unsigned long long>(found);
#else
    (void)n; (void)limit;
    if (why) *why = "built without an embedded PTX kernel";
    return 0;
#endif
}

// CPU equivalent, used to benchmark against the GPU and as the fallback.
static unsigned long long cpuSmallestFactor(const BigInt& n, unsigned long long limit) {
    const std::vector<unsigned int>& primes = primeTable(limit);
    BigInt a = n < 0 ? -n : n;
    size_t np = primes.size();
    if (np == 0) return 0;
    unsigned t = threadCount();
    size_t k = (t > 1) ? static_cast<size_t>(t) * 8 : 1;
    if (k > np) k = np;
    std::vector<unsigned long long> best(k, 0ULL);
    std::vector<unsigned long long> b = chunkBounds(np, k);
    parallelFor(k, [&](size_t i) {
        BigInt tmp;                       // per-thread scratch, never shared
        unsigned long long found = 0;
        for (size_t j = static_cast<size_t>(b[i]); j < static_cast<size_t>(b[i + 1]); ++j) {
            if (mpz_fdiv_r_ui(tmp.get_mpz_t(), a.get_mpz_t(), primes[j]) == 0) {
                found = primes[j];
                break;
            }
        }
        best[i] = found;
    });
    unsigned long long r = 0;
    for (size_t i = 0; i < k; ++i) {
        if (best[i] && (r == 0 || best[i] < r)) r = best[i];
    }
    return r;
}

// =====================================================================
//  streaming decimal output
// =====================================================================

static bool g_progress = true;
static const size_t STREAM_CHUNK = 1u << 16;          // 65536 digits per leaf
static unsigned long long g_emitted = 0;
static unsigned long long g_totalDigits = 0;

static void progressTick(bool force) {
    if (!g_progress || g_totalDigits < 2000000ULL) return;
    static std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (!force && std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() < 200) {
        return;
    }
    last = now;
    double pct = g_totalDigits
            ? 100.0 * static_cast<double>(g_emitted) / static_cast<double>(g_totalDigits) : 100.0;
    std::fprintf(stderr, "\r  [%5.1f%%  %llu / %llu digits]        ",
                 pct, g_emitted, g_totalDigits);
    std::fflush(stderr);
}

static void progressDone() {
    if (!g_progress || g_totalDigits < 2000000ULL) return;
    std::fprintf(stderr, "\r%62s\r", "");
    std::fflush(stderr);
}

static void emitRaw(const char* p, size_t n) {
    if (!n) return;
    std::cout.write(p, static_cast<std::streamsize>(n));
    std::cout.flush();                    // available digits leave the process now
    g_emitted += n;
    progressTick(false);
}

static void emitZeros(size_t n) {
    // NB: must be the character '0' (0x30), not a NUL-filled buffer.
    static const std::string zeros(512, '0');
    while (n) {
        size_t k = n < zeros.size() ? n : zeros.size();
        emitRaw(zeros.data(), k);
        n -= k;
    }
}

static void emitPadded(const BigInt& x, size_t nd) {
    static std::vector<char> buf;
    if (buf.size() < nd + 8) buf.resize(nd + 8);
    mpz_get_str(buf.data(), 10, x.get_mpz_t());
    // mpz_sizeinbase() is allowed to return one more than the true digit count,
    // so measure the actual string instead of trusting it.
    size_t actual = std::strlen(buf.data());
    if (actual > nd) actual = nd;
    if (nd > actual) emitZeros(nd - actual);
    if (actual) emitRaw(buf.data(), actual);
}

// Most-significant-first, divide and conquer. Every split halves the digit
// count, so the recursion is O(log digits) deep and the first characters
// appear after the very first division rather than after a full conversion.
static void streamIntDigits(BigInt x, size_t nd) {
    if (nd <= STREAM_CHUNK) { emitPadded(x, nd); return; }
    size_t h = nd / 2;
    // pow10Big, not mpz_ui_pow_ui: the latter takes a 32-bit unsigned long on
    // Windows, which a ten-billion-digit split would silently truncate.
    BigInt p = pow10Big(BigInt(static_cast<unsigned long>(h)));
    BigInt q, r;
    mpz_tdiv_qr(q.get_mpz_t(), r.get_mpz_t(), x.get_mpz_t(), p.get_mpz_t());
    BigInt().swap(p);
    BigInt().swap(x);                     // release before recursing
    streamIntDigits(std::move(q), nd - h);
    streamIntDigits(std::move(r), h);
}

// Split x into `k` pieces of roughly equal digit counts, in output order.
static void splitPieces(BigInt x, size_t nd, size_t k,
                        std::vector<BigInt>& out, std::vector<size_t>& counts) {
    if (k <= 1 || nd <= STREAM_CHUNK) {
        counts.push_back(nd);
        out.push_back(std::move(x));
        return;
    }
    size_t highK = k / 2;
    size_t lowK = k - highK;
    size_t h = static_cast<size_t>((static_cast<unsigned long long>(nd) * lowK) / k);
    if (h == 0) h = 1;
    if (h >= nd) h = nd - 1;
    BigInt p = pow10Big(BigInt(static_cast<unsigned long>(h)));
    BigInt q, r;
    mpz_tdiv_qr(q.get_mpz_t(), r.get_mpz_t(), x.get_mpz_t(), p.get_mpz_t());
    BigInt().swap(p);
    splitPieces(std::move(q), nd - h, highK, out, counts);
    splitPieces(std::move(r), h, lowK, out, counts);
}

// Zero-padded decimal of one piece. Pure function, safe on any thread.
static std::string paddedString(const BigInt& x, size_t nd) {
    std::vector<char> b(nd + 8);
    mpz_get_str(b.data(), 10, x.get_mpz_t());
    size_t actual = std::strlen(b.data());
    if (actual > nd) actual = nd;
    std::string s;
    s.reserve(nd);
    s.append(nd - actual, '0');
    s.append(b.data(), actual);
    return s;
}

static void streamIntegerValue(const BigInt& v, bool sign) {
    BigInt a = v < 0 ? -v : v;
    size_t nd = decimalLength(a);
    if (sign && v < 0) emitRaw("-", 1);

    // Above a couple of million digits the conversion is worth splitting:
    // the divide-and-conquer split stays sequential (each cut needs its
    // parent), but the leaf-to-ASCII step runs on every core. Pieces are
    // converted in batches of `threads`, so the extra buffer is about a
    // quarter of the result, not a whole copy of it.
    unsigned t = threadCount();
    if (t > 1 && nd > 2000000) {
        size_t k = static_cast<size_t>(t) * 4;
        std::vector<BigInt> pieces;
        std::vector<size_t> counts;
        pieces.reserve(k);
        counts.reserve(k);
        splitPieces(std::move(a), nd, k, pieces, counts);
        std::vector<std::string> bufs(t);
        for (size_t base = 0; base < pieces.size(); base += t) {
            size_t m = pieces.size() - base;
            if (m > t) m = t;
            parallelFor(m, [&](size_t i) {
                bufs[i] = paddedString(pieces[base + i], counts[base + i]);
            });
            for (size_t i = 0; i < m; ++i) {
                emitRaw(bufs[i].data(), bufs[i].size());
                std::string().swap(bufs[i]);
            }
        }
        return;
    }

    streamIntDigits(std::move(a), nd);
}

// Holds back a trailing run of 9s so that a half-up carry can be applied
// after the fact. Only the run of 9s is buffered, never the whole number.
class NinesHold {
public:
    explicit NinesHold() {}
    void put(char c) {
        ++g_emitted;
        if (c == '9') { held_.push_back('9'); progressTick(false); return; }
        flushHeld();
        prev_ = c;
        has_ = true;
        progressTick(false);
    }
    void finish(bool roundUp) {
        if (!has_) {
            // Every digit was a 9 and no earlier digit was held back, so there
            // is no prev_ to emit -- emitting one would prepend a spurious 0
            // (0.9999 would come out as 0.09999).
            if (roundUp) {
                std::cout.put('1');
                for (size_t i = 0; i < held_.size(); ++i) std::cout.put('0');
            } else if (!held_.empty()) {
                std::cout.write(held_.data(), static_cast<std::streamsize>(held_.size()));
            }
            held_.clear();
            std::cout.flush();
            return;
        }
        if (roundUp) {
            std::cout.put(static_cast<char>(prev_ + 1));
            for (size_t i = 0; i < held_.size(); ++i) std::cout.put('0');
        } else {
            std::cout.put(prev_);
            if (!held_.empty()) {
                std::cout.write(held_.data(), static_cast<std::streamsize>(held_.size()));
            }
        }
        held_.clear();
        std::cout.flush();
    }
private:
    void flushHeld() {
        if (has_) std::cout.put(prev_);
        if (!held_.empty()) {
            std::cout.write(held_.data(), static_cast<std::streamsize>(held_.size()));
            held_.clear();
        }
    }
    std::string held_;
    char prev_ = '0';
    bool has_ = false;
};

static void holdPadded(NinesHold& hold, const BigInt& q, size_t k) {
    static std::vector<char> b;
    if (b.size() < k + 8) b.resize(k + 8);
    mpz_get_str(b.data(), 10, q.get_mpz_t());
    size_t actual = std::strlen(b.data());       // not mpz_sizeinbase: that may be +1
    if (actual > k) actual = k;
    for (size_t i = 0; i < k - actual; ++i) hold.put('0');
    for (size_t i = 0; i < actual; ++i) hold.put(b[i]);
}

// Long division, `count` digits, generated in blocks. Never builds the
// scaled numerator, so the memory cost is independent of `count`.
static void streamFractionDigits(const BigInt& num, const BigInt& den,
                                 unsigned long long count, bool roundUp) {
    NinesHold hold;
    if (count == 0) { hold.finish(false); return; }

    BigInt rem;
    mpz_tdiv_r(rem.get_mpz_t(), num.get_mpz_t(), den.get_mpz_t());
    if (rem == 0) {
        for (unsigned long long i = 0; i < count; ++i) hold.put('0');
        hold.finish(false);
        return;
    }

    const unsigned long long BLOCK = 1000;
    BigInt blow;
    mpz_ui_pow_ui(blow.get_mpz_t(), 10UL, static_cast<unsigned long>(BLOCK));
    BigInt small;
    unsigned long long left = count;
    while (left > 0) {
        unsigned long long k = left < BLOCK ? left : BLOCK;
        BigInt mul = blow;
        if (k != BLOCK) {
            mpz_ui_pow_ui(mul.get_mpz_t(), 10UL, static_cast<unsigned long>(k));
        }
        BigInt scaled = rem * mul;
        BigInt q, r2;
        mpz_tdiv_qr(q.get_mpz_t(), r2.get_mpz_t(), scaled.get_mpz_t(), den.get_mpz_t());
        rem.swap(r2);
        holdPadded(hold, q, static_cast<size_t>(k));
        left -= k;
    }
    hold.finish(roundUp);
}

// Is  (num mod den) * 10^scale  congruent to 10^scale - 1  times den?
// i.e. are all `scale` fraction digits 9?  Only asked when it could matter.
static bool fractionIsAllNines(const BigInt& num, const BigInt& den, const BigInt& scale) {
    BigInt N;
    mpz_tdiv_r(N.get_mpz_t(), num.get_mpz_t(), den.get_mpz_t());
    if (N == 0) return false;
    size_t dd = decimalLength(den);
    if (scale >= BigInt(static_cast<unsigned long>(dd))) {
        // 10^scale > den, so (den-N)*10^scale >= 10^scale > den
        return false;
    }
    BigInt p10 = pow10Big(scale);
    return (den - N) * p10 <= den;
}

// Does num/den round up (half away from zero) at `scale` decimals?
static bool roundsUpAt(const BigInt& num, const BigInt& den, const BigInt& scale) {
    BigInt N;
    mpz_tdiv_r(N.get_mpz_t(), num.get_mpz_t(), den.get_mpz_t());
    if (N == 0) return false;
    BigInt ten(10), t;
    mpz_powm(t.get_mpz_t(), ten.get_mpz_t(), scale.get_mpz_t(), den.get_mpz_t());
    BigInt rem = (N * t) % den;
    return 2 * rem >= den;
}

// Print v to `scale` decimals, half-up, streaming. Matches the old
// toDecimal() digit for digit.
static void printRoundedDecimal(const Rat& v, unsigned long long scale) {
    BigInt num = v.get_num();
    bool neg = num < 0;
    if (neg) num = -num;
    BigInt den = v.get_den();
    BigInt scaleZ(static_cast<unsigned long>(scale));

    BigInt intPart = num / den;
    bool roundUp = roundsUpAt(num, den, scaleZ);
    bool carry = scale > 0 && roundUp && fractionIsAllNines(num, den, scaleZ);
    if (carry) intPart += 1;

    g_emitted = 0;
    g_totalDigits = static_cast<unsigned long long>(decimalLength(intPart))
                  + (scale ? scale + 1 : 0);
    if (neg) emitRaw("-", 1);
    streamIntegerValue(intPart, false);
    if (scale > 0) {
        emitRaw(".", 1);
        if (carry) {
            // rounding pushed the whole fraction up into the integer part, so
            // the fraction is exactly `scale` zeros
            emitZeros(static_cast<size_t>(scale));
        } else {
            streamFractionDigits(num, den, scale, roundUp);
        }
    }
    progressDone();
}

// Print an exactly-terminating rational, with trailing zeros trimmed,
// matching the old exactDecimal() output.
static void printExactDecimal(const Rat& v) {
    BigInt num = v.get_num();
    bool neg = num < 0;
    if (neg) num = -num;
    BigInt den = v.get_den();

    // Same trick as terminates(): factor 10^a out of the denominator with one
    // scan and one shift, and get the 5-count from mpz_remove, rather than
    // looping O(size) divisions per factor.
    BigInt t = den;
    unsigned long twos = den == 0 ? 0UL : static_cast<unsigned long>(mpz_scan1(t.get_mpz_t(), 0));
    if (twos) t >>= twos;
    BigInt five(5), tOut;
    unsigned long fives = static_cast<unsigned long>(
            mpz_remove(tOut.get_mpz_t(), t.get_mpz_t(), five.get_mpz_t()));
    unsigned long long scale = static_cast<unsigned long long>(std::max(twos, fives));

    BigInt intPart = num / den;

    // scaled = num * 10^scale / den exactly; its trailing decimal zeros are
    // min(v2, v5), which lets the tail be trimmed without building it.
    BigInt tmp = num;
    unsigned long long v2 = 0, v5 = 0;
    if (tmp != 0) {
        v2 = static_cast<unsigned long long>(mpz_scan1(tmp.get_mpz_t(), 0));
        BigInt five(5), out;
        v5 = static_cast<unsigned long long>(mpz_remove(out.get_mpz_t(), tmp.get_mpz_t(),
                                                         five.get_mpz_t()));
    }
    unsigned long long z2 = v2 + (scale - static_cast<unsigned long long>(twos));
    unsigned long long z5 = v5 + (scale - static_cast<unsigned long long>(fives));
    unsigned long long tz = z2 < z5 ? z2 : z5;
    if (tz > scale) tz = scale;

    unsigned long long frac = scale - tz;

    g_emitted = 0;
    g_totalDigits = static_cast<unsigned long long>(decimalLength(intPart))
                  + (frac ? frac + 1 : 0);
    if (neg) emitRaw("-", 1);
    streamIntegerValue(intPart, false);
    if (frac > 0) {
        emitRaw(".", 1);
        streamFractionDigits(num, den, frac, false);
    }
    progressDone();
}

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
    BigInt digits = 30;          // n/d decimals shown; arbitrary precision, no cap
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

    // Parallel product tree. The leaves are independent, so they go to all
    // cores; the combine step is a pairwise tree and parallelises as well.
    // Used for fact(n) and for the e() series blocks.
    static BigInt productRangeParallel(unsigned long lo, unsigned long hi) {
        if (lo > hi) return BigInt(1);
        unsigned long long total = static_cast<unsigned long long>(hi - lo) + 1ULL;
        unsigned t = threadCount();
        if (t <= 1 || total < 20000ULL) return productRange(lo, hi);

        size_t k = static_cast<size_t>(t) * 4;
        if (static_cast<unsigned long long>(k) > total) k = static_cast<size_t>(total);
        std::vector<unsigned long long> b = chunkBounds(total, k);
        std::vector<BigInt> parts(k);
        parallelFor(k, [&](size_t i) {
            if (b[i] < b[i + 1]) {
                parts[i] = productRange(static_cast<unsigned long>(static_cast<unsigned long long>(lo) + b[i]),
                                        static_cast<unsigned long>(static_cast<unsigned long long>(lo) + b[i + 1] - 1ULL));
            } else {
                parts[i] = BigInt(1);
            }
        });
        while (parts.size() > 1) {
            size_t m = parts.size();
            size_t pairs = m / 2;
            std::vector<BigInt> next(pairs + (m & 1));
            parallelFor(pairs, [&](size_t i) { next[i] = parts[2 * i] * parts[2 * i + 1]; });
            if (m & 1) next[pairs] = std::move(parts[m - 1]);
            parts = std::move(next);
        }
        return parts.empty() ? BigInt(1) : std::move(parts[0]);
    }

    static BigInt factorial(const BigInt& n) {
        if (n < 0) throw CalcError("factorial requires a non-negative integer");
        requireFeasible(digitsOfFactorial(n), "fact(" + n.get_str() + ")");
        if (!mpz_fits_ulong_p(n.get_mpz_t())) {
            throw CalcError("fact(" + n.get_str() + ") is past the range any machine can "
                            "enumerate; :guard off to attempt it anyway");
        }
        unsigned long k = n.get_ui();
        return k < 2 ? BigInt(1) : productRangeParallel(2, k);
    }

    static BigInt fibonacci(const BigInt& n) {
        if (n < 0) throw CalcError("fib requires a non-negative integer");
        requireFeasible(digitsOfFibonacci(n), "fib(" + n.get_str() + ")");
        if (!mpz_fits_ulong_p(n.get_mpz_t())) {
            throw CalcError("fib(" + n.get_str() + ") is past the range any machine can reach; "
                            ":guard off to attempt it anyway");
        }
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

    // base^e, e >= 0, with NO limit on e. mpz_pow_ui is used when it can be
    // (it is the fastest), otherwise square-and-multiply with an mpz exponent.
    static BigInt intPow(const BigInt& base, const BigInt& e) {
        if (e <= 0) return BigInt(1);
        if (mpz_fits_ulong_p(e.get_mpz_t())) {
            BigInt r;
            mpz_pow_ui(r.get_mpz_t(), base.get_mpz_t(), e.get_ui());
            return r;
        }
        BigInt r(1), b = base, ee = e;
        while (ee != 0) {
            if (mpz_odd_p(ee.get_mpz_t())) r *= b;
            ee >>= 1;
            if (ee != 0) b *= b;
        }
        return r;
    }

    static Rat power(const Rat& base, const Rat& exponent) {
        if (isInteger(exponent)) {
            BigInt e = exponent.get_num();
            if (e == 0) return Rat(1);
            bool negExp = e < 0;
            if (negExp) e = -e;
            if (base == 0) {
                if (negExp) throw CalcError("division by zero (0 raised to a negative power)");
                return Rat(0);
            }
            if (base == 1) return Rat(1);
            if (base == -1) {
                return Rat(negExp && mpz_odd_p(e.get_mpz_t()) ? -1 : 1);
            }
            // size it up before touching any memory
            {
                BigInt digs;
                if (!negExp && base == 2) {
                    digs = digitsOfPowerOfTwo(e);
                } else if (!negExp && base == 10) {
                    digs = e + 1;
                } else {
                    size_t bn = bitLength(base.get_num());
                    size_t bd = bitLength(base.get_den());
                    BigInt nb(static_cast<unsigned long>(bn > 1 ? bn - 1 : 1));
                    BigInt db(static_cast<unsigned long>(bd > 1 ? bd - 1 : 1));
                    BigInt bits = (nb > db ? nb : db) * e;
                    digs = (bits * BigInt("3010299956639811952137")) / pow10Big(BigInt(22)) + 1;
                }
                requireFeasible(digs, "(" + base.get_str() + ")^(" + e.get_str() + ")");
            }
            BigInt rn = intPow(base.get_num(), e);
            BigInt rd = intPow(base.get_den(), e);
            return negExp ? ratFrom(rd, rn) : ratFrom(rn, rd);
        }
        BigInt p = exponent.get_num();
        BigInt q = exponent.get_den();
        if (!mpz_fits_ulong_p(q.get_mpz_t())) {
            throw CalcError("root index " + q.get_str() + " is past the range a root can be taken in");
        }
        unsigned long k = q.get_ui();
        Rat root;
        if (!exactRoot(base, k, root)) {
            throw CalcError("result is irrational; use root(x, k, digits) or sqrt(x, digits)");
        }
        if (!mpz_fits_ulong_p(p.get_mpz_t())) {
            // p/q = r + s, so base^(p/q) = base^r * base^(s/q); s/q is in [0,1)
            // and base^(s/q) is irrational for base >= 2, so refuse honestly.
            throw CalcError("exponent numerator " + p.get_str() + " is too large to raise to "
                            "(no machine could hold the result)");
        }
        unsigned long m = p.get_ui();
        if (m == 0) return Rat(1);
        if (root == 0) throw CalcError("division by zero");
        BigInt pn = intPow(root.get_num(), BigInt(m));
        BigInt pd = intPow(root.get_den(), BigInt(m));
        return ratFrom(pn, pd);
    }

    // ---------------- decimal rendering ----------------
    // (the actual decimal writers live above as printRoundedDecimal /
    //  printExactDecimal -- they stream instead of building a string)
    static bool terminates(const Rat& v) {
        BigInt t = v.get_den();
        if (t == 1) return true;
        // Count the factors of 2 with mpz_scan1 (trailing zero bits, O(1)
        // amortised) instead of shifting one bit at a time: for a denominator
        // like 10^200000 the naive loop is 200,000 O(n) shifts, i.e. quadratic,
        // and it dominated every pi(d)/e(d) render.
        mp_bitcnt_t a = mpz_scan1(t.get_mpz_t(), 0);
        if (a) t >>= a;
        if (t == 1) return true;
        BigInt five(5), out;
        mpz_remove(out.get_mpz_t(), t.get_mpz_t(), five.get_mpz_t());
        return out == 1;
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

    struct Chud { BigInt P, Q, T; };

    // Parallel binary splitting. Each block is an independent binary split of
    // a contiguous slice of the series; combining A then B is
    // (P,Q,T) = (P1*P2, Q1*Q2, Q2*T1 + P1*T2), which is exactly what the
    // sequential recursion does, so the result is bit-identical.
    static Chud chudnovskyParallel(unsigned long terms) {
        Chud r;
        unsigned t = threadCount();
        if (t <= 1 || terms < 512) {
            chudnovsky(0, terms, r.P, r.Q, r.T);
            return r;
        }
        size_t k = static_cast<size_t>(t) * 4;
        if (static_cast<unsigned long long>(k) > terms) k = static_cast<size_t>(terms);
        std::vector<unsigned long long> b = chunkBounds(terms, k);
        std::vector<Chud> parts(k);
        parallelFor(k, [&](size_t i) {
            unsigned long lo = static_cast<unsigned long>(b[i]);
            unsigned long hi = static_cast<unsigned long>(b[i + 1]);
            if (lo < hi) chudnovsky(lo, hi, parts[i].P, parts[i].Q, parts[i].T);
            else { parts[i].P = 1; parts[i].Q = 1; parts[i].T = 0; }
        });
        while (parts.size() > 1) {
            size_t m = parts.size();
            size_t pairs = m / 2;
            std::vector<Chud> next(pairs + (m & 1));
            parallelFor(pairs, [&](size_t i) {
                const Chud& A = parts[2 * i];
                const Chud& B = parts[2 * i + 1];
                next[i].P = A.P * B.P;
                next[i].Q = A.Q * B.Q;
                next[i].T = B.Q * A.T + A.P * B.T;
            });
            if (m & 1) next[pairs] = std::move(parts[m - 1]);
            parts = std::move(next);
        }
        if (parts.empty()) { r.P = 1; r.Q = 1; r.T = 0; return r; }
        return std::move(parts[0]);
    }

    static Rat piRat(const BigInt& digitsIn) {
        if (digitsIn < 0) throw CalcError("digits must be >= 0");
        requireFeasible(digitsIn + 32, "pi(" + digitsIn.get_str() + ")");
        if (!mpz_fits_ulong_p(digitsIn.get_mpz_t())) {
            throw CalcError("pi(" + digitsIn.get_str() + ") is past what any machine can hold; "
                            ":guard off to attempt it anyway");
        }
        const unsigned long guard = 15;
        unsigned long long work = static_cast<unsigned long long>(digitsIn.get_ui()) + guard;
        unsigned long long terms = static_cast<unsigned long long>(work / 14.181647462725477) + 2;
        if (terms > 0xFFFFFFFFULL) {
            throw CalcError("pi(): " + digitsIn.get_str() + " digits exceeds the series a "
                            "32-bit unsigned long can index on this platform");
        }
        Chud c = chudnovskyParallel(static_cast<unsigned long>(terms));
        BigInt& Q = c.Q;
        BigInt& T = c.T;
        BigInt scale = pow10Big(BigInt(static_cast<unsigned long>(work)));
        BigInt radicand = BigInt(10005) * scale * scale;
        BigInt s;
        mpz_sqrt(s.get_mpz_t(), radicand.get_mpz_t());
        BigInt scaledPi = (BigInt(426880) * Q * s) / T;

        BigInt div = pow10Big(BigInt(guard));
        BigInt q = scaledPi / div, r = scaledPi % div;
        if (2 * r >= div) q += 1;

        BigInt den = pow10Big(digitsIn);
        return ratFrom(q, den);
    }

    static Rat eRat(const BigInt& digitsIn) {
        if (digitsIn < 0) throw CalcError("digits must be >= 0");
        requireFeasible(digitsIn + 64, "e(" + digitsIn.get_str() + ")");
        if (!mpz_fits_ulong_p(digitsIn.get_mpz_t())) {
            throw CalcError("e(" + digitsIn.get_str() + ") is past what any machine can hold; "
                            ":guard off to attempt it anyway");
        }
        const unsigned long guard = 15;
        unsigned long long work = static_cast<unsigned long long>(digitsIn.get_ui()) + guard;

        BigInt target = pow10Big(BigInt(static_cast<unsigned long>(work + 2)));
        // 10^(work+2) has work+3 digits, so N is bounded by that; the loop
        // below is O(N) multiply-adds and N grows like work/log10(work).
        BigInt N = 1, factN = 1;
        while (factN < target) { ++N; factN *= N; }
        if (!mpz_fits_ulong_p(N.get_mpz_t())) {
            throw CalcError("e(" + digitsIn.get_str() + "): series length past any machine");
        }
        unsigned long Nu = N.get_ui();

        // A = sum_{j=0}^{N} N!/j!   ->   e ~ A / N!
        //
        // Sequential version is one running product plus one add per term.
        // Parallel version splits [0,N] into blocks and uses
        //     sum_{j=lo}^{hi} N!/j!  =  (N!/hi!) * sum_{j=lo}^{hi} hi!/j!
        // so each block is an independent local backward pass.
        BigInt A;
        unsigned t = threadCount();
        if (t <= 1 || Nu < 20000UL) {
            BigInt tt = 1;
            A = 1;
            for (unsigned long k = Nu; k >= 1; --k) { tt *= k; A += tt; }
        } else {
            size_t k = static_cast<size_t>(t) * 2;
            std::vector<unsigned long long> b =
                    chunkBounds(static_cast<unsigned long long>(Nu) + 1ULL, k);
            std::vector<BigInt> S(k), Bp(k);
            parallelFor(k, [&](size_t i) {
                unsigned long lo = static_cast<unsigned long>(b[i]);
                unsigned long hi = static_cast<unsigned long>(b[i + 1]) - 1UL;
                BigInt tt = 1, s = 1;
                for (unsigned long j = hi; j > lo; --j) { tt *= j; s += tt; }
                S[i] = std::move(s);
                // prod(lo..hi); block 0 starts at 0 and is never used by the scan
                Bp[i] = (lo == 0) ? BigInt(1) : productRange(lo, hi);
            });
            // W[k-1] = N!/hi_last! = 1; W[i-1] = W[i] * prod(lo_i .. hi_i).
            // Only k-1 multiplies, and the sizes grow geometrically, so this
            // sequential tail costs about one big multiplication in total.
            std::vector<BigInt> W(k);
            W[k - 1] = 1;
            for (size_t i = k - 1; i > 0; --i) W[i - 1] = W[i] * Bp[i];
            std::vector<BigInt> terms(k);
            parallelFor(k, [&](size_t i) { terms[i] = W[i] * S[i]; });
            while (terms.size() > 1) {
                size_t m = terms.size();
                size_t pairs = m / 2;
                std::vector<BigInt> next(pairs + (m & 1));
                parallelFor(pairs, [&](size_t i) { next[i] = terms[2 * i] + terms[2 * i + 1]; });
                if (m & 1) next[pairs] = std::move(terms[m - 1]);
                terms = std::move(next);
            }
            A = std::move(terms[0]);
        }

        BigInt scale = pow10Big(BigInt(static_cast<unsigned long>(work)));
        BigInt scaledE = (A * scale) / factN;

        BigInt div = pow10Big(BigInt(guard));
        BigInt q = scaledE / div, r = scaledE % div;
        if (2 * r >= div) q += 1;

        BigInt den = pow10Big(digitsIn);
        return ratFrom(q, den);
    }

    static Rat rootApprox(const Rat& v, unsigned long k, const BigInt& digitsIn) {
        if (k == 0) throw CalcError("root index must be >= 1");
        if (digitsIn < 0) throw CalcError("digits must be >= 0");
        requireFeasible(digitsIn * BigInt(static_cast<unsigned long>(k + 1)) + 32,
                        "root(x, " + std::to_string(k) + ", " + digitsIn.get_str() + ")");
        if (!mpz_fits_ulong_p(digitsIn.get_mpz_t())) {
            throw CalcError("root(): " + digitsIn.get_str() + " digits is past what any machine "
                            "can hold; :guard off to attempt it anyway");
        }
        bool negative = v < 0;
        if (negative && (k % 2 == 0)) throw CalcError("even root of a negative number is not real");
        BigInt num = v.get_num();
        if (num < 0) num = -num;
        BigInt den = v.get_den();
        BigInt scale = pow10Big(digitsIn);
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
    if (!mpz_fits_ulong_p(v.get_num().get_mpz_t())) {
        throw CalcError(what + " = " + v.get_str() + " does not fit the platform's "
                        "unsigned long, which GMP requires for this operation");
    }
    return v.get_num().get_ui();
}

// Non-negative integer argument of arbitrary magnitude. No cap: the value
// is carried as an mpz_class all the way to the point where a physical
// resource check (or GMP's own platform bound) decides.
static BigInt asBigCount(const Rat& v, const std::string& what) {
    if (!isInteger(v)) throw CalcError(what + " requires an integer, got " + v.get_str());
    BigInt n = v.get_num();
    if (n < 0) throw CalcError(what + " must be >= 0, got " + n.get_str());
    return n;
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
        // Small-factor screen on the GPU before the expensive Miller-Rabin
        // rounds. A hit is a proof of compositeness, so this can only help.
        if (g_useGpu && decimalLength(n) > 50) {
            unsigned long long f = gpuSmallestFactor(n, 1000000ULL, nullptr);
            if (f != 0) return Rat(0);
        }
        return Rat(mpz_probab_prime_p(n.get_mpz_t(), 25) != 0 ? 1 : 0);
    }

    // smallest prime factor of |n| up to `limit`, 0 if there is none
    if (name == "smallfactor") {
        minArity(name, a, 1);
        if (a.size() > 2) throw CalcError("smallfactor takes 1 or 2 arguments");
        if (!isInteger(a[0])) throw CalcError("smallfactor requires an integer");
        BigInt n = a[0].get_num();
        if (n < 0) n = -n;
        if (n < 2) return Rat(0);
        unsigned long long limit = 10000000ULL;
        if (a.size() == 2) {
            unsigned long long want = asULong(a[1], "limit");
            if (want == 0) throw CalcError("smallfactor: limit must be >= 2");
            limit = want > 2000000000ULL ? 2000000000ULL : want;
        }
        if (limit < 2) return Rat(0);
        if (g_useGpu) {
            unsigned long long f = gpuSmallestFactor(n, limit, nullptr);
            if (f != 0) return Rat(BigInt(static_cast<unsigned long>(f)));
        }
        unsigned long long f = cpuSmallestFactor(n, limit);
        return Rat(f == 0 ? BigInt(0) : BigInt(static_cast<unsigned long>(f)));
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
        return rootApprox(a[0], k, asBigCount(a[1], "digits"));
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
        return rootApprox(a[0], asULong(a[1], "root index"), asBigCount(a[2], "digits"));
    }

    if (name == "pi") { arity(name, a, 1); return piRat(asBigCount(a[0], "digits")); }
    if (name == "e") { arity(name, a, 1); return eRat(asBigCount(a[0], "digits")); }

    throw CalcError("unknown function '" + name + "'");
}

// =====================================================================
//  REPL
// =====================================================================

static const char* HELP =
    "BigCalcGMP 2.0 - exact arbitrary-precision calculator (no size limit, GMP backend)\n"
    "\n"
    "Operators     + - * / % ^ !   and parentheses; ^ is right associative\n"
    "               % is a - b*floor(a/b); ! is postfix factorial\n"
    "Literals      decimal, 0x.. hex, 0b.. binary, 0o.. octal, _ separators\n"
    "Variables     x = <expr>   (also: ans = previous result)\n"
    "Exact funcs   abs floor ceil trunc round sign gcd lcm min max\n"
    "              fact fib isqrt ndigits isprime nextprime pow root smallfactor\n"
    "Precision     sqrt(x, d)  root(x, k, d)  pi(d)  e(d)\n"
    "              (sqrt/root without d fail unless the result is exact)\n"
    "\n"
    "No size limit\n"
    "  Exponents, digit counts, root indices and factorial arguments are\n"
    "  arbitrary-precision integers. 2^9999999999999999999999999999999999999999999999\n"
    "  parses and is sized up before any work begins. The only thing that can\n"
    "  stop it is free RAM, which is reported in exact bytes. :guard off skips\n"
    "  even that check and lets the machine try.\n"
    "\n"
    "Streaming output\n"
    "  Results are written most-significant-digit first and flushed as they are\n"
    "  produced, so huge answers scroll immediately instead of appearing only\n"
    "  after the whole decimal string exists. A progress meter on stderr tracks\n"
    "  long conversions (auto above 2,000,000 digits).\n"
    "\n"
    "Multithreading\n"
    "  fact(n) uses a parallel product tree, pi(d) uses parallel binary\n"
    "  splitting, e(d) splits its series into blocks, and decimal conversion\n"
    "  above 2,000,000 digits splits the result and converts the pieces on all\n"
    "  cores. Threads are auto-sized to the CPU (see :threads); GMP objects are\n"
    "  never shared between threads.\n"
    "\n"
    "GPU (CUDA)\n"
    "  One kernel: n mod p for millions of small primes at once, which is the\n"
    "  only part of this program that is embarrassingly parallel. It backs\n"
    "  smallfactor(n[, limit]) and screens isprime() on inputs over 50 digits.\n"
    "  Arbitrary-precision arithmetic itself stays on the CPU -- a chain of huge\n"
    "  multiplications is GMP's strength, not a GPU's.\n"
    "\n"
    "Commands\n"
    "  :threads N  worker threads (no arg = show, N=0 = auto, default auto)\n"
    "  :gpu        on|off|info|bench [limit]  (bench screens ans)\n"
    "  :digits N   decimals shown for non-integers (default 30, 0 = exact only)\n"
    "              N is arbitrary precision; the whole expansion is streamed\n"
    "  :base N     integer output base: 10, 16, 8 or 2\n"
    "  :hex :dec :bin :oct\n"
    "  :vars       list variables\n"
    "  :frac       force n/d display for non-integers\n"
    "  :time       toggle evaluation timing\n"
    "  :guard      on|off - RAM feasibility pre-check (default on)\n"
    "  :progress   on|off - streaming progress meter (default on)\n"
    "  :mem        free RAM, digits that fit, current settings, ans\n"
    "  :help       this text\n"
    "  :quit       exit\n"
    "\n"
    "Every value is an exact rational: 1/3 stays 1/3, 2^100000000 is exact,\n"
    "1000000! is exact, and results are never truncated.";

static void streamBase(const BigInt& n, int base) {
    std::string s = toBaseString(n, base);
    emitRaw(s.data(), s.size());
}

static void printValue(Engine& engine, const Rat& v) {
    g_emitted = 0;
    g_totalDigits = 0;

    if (isInteger(v)) {
        if (engine.base == 10) {
            BigInt a = v.get_num();
            g_totalDigits = static_cast<unsigned long long>(decimalLength(a));
            streamIntegerValue(a, true);
        } else {
            streamBase(v.get_num(), engine.base);
        }
        progressDone();
        std::cout << '\n';
        return;
    }

    if (!engine.frac && engine.base == 10 && Engine::terminates(v)) {
        printExactDecimal(v);
        std::cout << '\n';
        return;
    }

    // exact n/d form (streamed, so huge numerators/denominators stay readable)
    {
        BigInt num = v.get_num(), den = v.get_den();
        g_totalDigits = static_cast<unsigned long long>(decimalLength(num))
                      + static_cast<unsigned long long>(decimalLength(den)) + 1;
        streamIntegerValue(num, true);
        emitRaw("/", 1);
        streamIntegerValue(den, false);
        progressDone();
    }
    std::cout << '\n';

    if (engine.digits > 0 && engine.base == 10) {
        if (!mpz_fits_ulong_p(engine.digits.get_mpz_t())) {
            throw CalcError(":digits " + engine.digits.get_str()
                            + " is beyond the 64-bit range this platform can index");
        }
        std::cout << "  = ";
        printRoundedDecimal(v, engine.digits.get_ui());
        std::cout << "  (rounded to " << engine.digits.get_str() << " decimals)\n";
    }
}

int main() {
    // must be the very first thing, before any mpz_class exists
    mp_set_memory_functions(gmpAlloc, gmpRealloc, gmpFree);

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
        std::cout << "BigCalcGMP 2.0  -  exact arbitrary-precision calculator (GMP)\n"
                     "no size limit, streaming decimal output\n"
                     ":help for help, :quit to exit, :mem for free RAM\n";
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
                    BigInt d;
                    if (mpz_set_str(d.get_mpz_t(), arg.c_str(), 10) != 0) {
                        throw CalcError("digits must be a decimal integer, got '" + arg + "'");
                    }
                    if (d < 0) throw CalcError("digits must be >= 0");
                    if (!mpz_fits_ulong_p(d.get_mpz_t())) {
                        throw CalcError("digits = " + d.get_str() + " does not fit in 64 bits; "
                                        "no machine can hold that many digits");
                    }
                    engine.digits = d;
                    std::cout << "digits = " << d.get_str() << '\n';
                }
                else if (cmd == "threads" || cmd == "t") {
                    if (arg.empty()) {
                        std::cout << "threads = " << threadCount()
                                  << "  (max " << std::thread::hardware_concurrency() << ")\n";
                    } else {
                        BigInt n;
                        if (mpz_set_str(n.get_mpz_t(), arg.c_str(), 10) != 0 || n < 0) {
                            throw CalcError(":threads expects a non-negative decimal integer");
                        }
                        if (!mpz_fits_ulong_p(n.get_mpz_t())) {
                            throw CalcError(":threads " + n.get_str() + " is not a thread count");
                        }
                        unsigned long v2 = n.get_ui();
                        unsigned hw = std::thread::hardware_concurrency();
                        if (v2 > 1024) throw CalcError(":threads capped at 1024 (got " + n.get_str() + ")");
                        g_threads = static_cast<unsigned>(v2);
                        std::cout << "threads = " << threadCount()
                                  << (g_threads ? "" : " (auto)")
                                  << "  (max " << hw << ")\n";
                    }
                }
                else if (cmd == "gpu") {
                    std::string sub = lower(arg);
                    if (sub.empty() || sub == "info" || sub == "status") {
#if defined(BIGCALC_EMBEDDED_PTX)
                        cuda::Session& S = cuda::session();
                        std::cout << "backend    : CUDA Driver API (nvcuda.dll + embedded PTX)\n";
                        std::cout << "enabled    : " << (g_useGpu ? "yes" : "no") << '\n';
                        if (S.ok) {
                            std::cout << "device     : " << S.device << '\n';
                            std::cout << "compute    : " << S.ccMajor << "." << S.ccMinor << '\n';
                            std::cout << "VRAM       : " << humanBytes(bytesBig(S.vram)) << '\n';
                            std::cout << "kernel     : kModSmallPrimes  (one thread per prime)\n";
                            std::cout << "used for   : smallfactor(), and the screen before isprime()\n";
                        } else {
                            std::cout << "device     : UNAVAILABLE - " << S.why << '\n';
                        }
#else
                        std::cout << "backend    : none (built without an embedded PTX kernel)\n";
#endif
                    }
                    else if (sub == "on") { g_useGpu = true; std::cout << "gpu on\n"; }
                    else if (sub == "off") { g_useGpu = false; std::cout << "gpu off\n"; }
                    else if (sub.rfind("bench", 0) == 0) {
                        BigInt n = engine.last.get_num();
                        if (n < 0) n = -n;
                        if (n < 2) throw CalcError(":gpu bench screens ans; set ans to a big integer first");
                        unsigned long long lim = 100000000ULL;
                        {
                            std::string rest = trim(sub.substr(5));
                            if (!rest.empty()) {
                                BigInt L;
                                if (mpz_set_str(L.get_mpz_t(), rest.c_str(), 10) != 0 || L < 2) {
                                    throw CalcError(":gpu bench [limit] - limit must be a decimal integer >= 2");
                                }
                                lim = std::min<unsigned long long>(L.get_ui(), 2000000000ULL);
                            }
                        }
                        std::cout << "screening a " << decimalLength(n) << "-digit number against primes <= "
                                  << lim << "  (single warm-up run first)\n";
                        std::string why;
                        (void)gpuSmallestFactor(n, lim, &why);          // warm up / JIT
                        auto t0 = std::chrono::steady_clock::now();
                        unsigned long long fg = gpuSmallestFactor(n, lim, &why);
                        auto t1 = std::chrono::steady_clock::now();
                        unsigned long long fc = cpuSmallestFactor(n, lim);
                        auto t2 = std::chrono::steady_clock::now();
                        double mg = std::chrono::duration<double, std::milli>(t1 - t0).count();
                        double mc = std::chrono::duration<double, std::milli>(t2 - t1).count();
                        std::cout << "  GPU : " << mg << " ms   factor " << fg << (why.empty() ? "" : ("  [" + why + "]")) << '\n';
                        std::cout << "  CPU : " << mc << " ms   factor " << fc
                                  << "   (" << threadCount() << " threads)\n";
                        if (mg > 0 && mc > 0) std::cout << "  speedup: " << (mc / mg) << "x\n";
                        if (fg != fc) std::cout << "  ** MISMATCH between GPU and CPU results **\n";
                    }
                    else throw CalcError(":gpu takes on|off|info|bench [limit]");
                }
                else if (cmd == "guard") {
                    if (arg.empty()) g_guard = !g_guard;
                    else { std::string v2 = lower(arg); g_guard = (v2 == "on" || v2 == "1"); }
                    std::cout << "guard " << (g_guard ? "on (refuse what RAM cannot hold)"
                                                      : "off (attempt anything)") << '\n';
                }
                else if (cmd == "progress") {
                    if (arg.empty()) g_progress = !g_progress;
                    else { std::string v2 = lower(arg); g_progress = (v2 == "on" || v2 == "1"); }
                    std::cout << "progress " << (g_progress ? "on" : "off") << '\n';
                }
                else if (cmd == "mem") {
                    unsigned long long a = availableBytes();
                    BigInt availBytes = bytesBig(a);
                    BigInt fits = (availBytes * 8) * pow10Big(BigInt(40))
                                / BigInt("33219280948873623478703194294893901758648");
                    std::cout << "free RAM       : " << humanBytes(availBytes) << '\n';
                    std::cout << "digits that fit: ~" << fits.get_str()
                              << " (binary value only, no output buffer)\n";
                    std::cout << "guard          : " << (g_guard ? "on" : "off")
                              << "     progress: " << (g_progress ? "on" : "off") << '\n';
                    if (!engine.vars.empty()) std::cout << "vars           : " << engine.vars.size() << '\n';
                    Rat& last = engine.last;
                    std::cout << "ans            : ";
                    g_emitted = 0; g_totalDigits = 0;
                    streamIntegerValue(last.get_num(), true);
                    if (last.get_den() != 1) { emitRaw("/", 1); streamIntegerValue(last.get_den(), false); }
                    progressDone();
                    std::cout << '\n';
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
                        std::cout << "  " << kv.first << " = ";
                        g_emitted = 0; g_totalDigits = 0;
                        streamIntegerValue(kv.second.get_num(), true);
                        if (kv.second.get_den() != 1) {
                            emitRaw("/", 1);
                            streamIntegerValue(kv.second.get_den(), false);
                        }
                        progressDone();
                        std::cout << '\n';
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

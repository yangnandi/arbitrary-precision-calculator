// gpu_screen.cu - CUDA kernels for BigCalcGMP.
//
// Compiled to PTX by nvcc, embedded in BigCalcGMP.cpp as a string, and loaded
// at runtime through the CUDA Driver API (nvcuda.dll). That keeps the whole
// calculator a single MinGW-built executable: nothing here needs the CUDA
// runtime, a .lib, or an MSVC-ABI shared library.
//
// Build:
//   nvcc -ptx -arch=compute_120 -O3 -o gpu_screen.ptx gpu_screen.cu
//   (see ptx_to_header.py, driven by build.cmd)
//
// What belongs on the GPU here
// ----------------------------
// Arbitrary-precision arithmetic itself does not: the hot path is a chain of
// huge multiplications, which is squarely GMP's territory and is latency- and
// bandwidth-bound on a GPU, not throughput-bound. What *is* embarrassingly
// parallel is screening one huge number against a very large table of small
// primes -- millions of independent (n mod p) reductions, no data flowing
// between them. That is the kernel below, and it is the whole GPU story.

// One thread per small prime. Horner over the 32-bit limbs of the big number:
//   r = 0; for limb from most significant: r = (r << 32 | limb) mod p
// The big number is little-endian, so we walk it backwards. All threads read
// the same limb at the same time, which the constant cache handles well.
//
// primes are restricted to p < 2^31 so that (r << 32 | limb) cannot overflow
// the 64-bit accumulator.
extern "C" __global__ void kModSmallPrimes(
        const unsigned int* __restrict__ limbs,
        int nlimbs,
        const unsigned int* __restrict__ primes,
        int nprimes,
        unsigned int* __restrict__ residues,
        unsigned int* __restrict__ foundMin)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= nprimes) return;

    unsigned long long p = primes[i];
    unsigned long long r = 0;
    for (int j = nlimbs - 1; j >= 0; --j) {
        r = ((r << 32) | (unsigned long long)limbs[j]) % p;
    }
    if (residues) residues[i] = (unsigned int)r;
    // Smallest prime that divides the number, without copying the table back.
    if (r == 0) atomicMin(foundMin, primes[i]);
}

// Segment-parallel variant used when the number is so long that one thread
// walking every limb becomes the bottleneck. The number is cut into `nseg`
// contiguous limb ranges; thread (i, s) reduces segment s modulo primes[i] and
// the segments are combined as sum(partial_s * (2^(32*len_s) mod p)) by the
// host -- actually by this second pass, so only one table is transferred.
//
// partial[i * nseg + s] = value of limbs [off_s, off_s+len_s) mod primes[i]
extern "C" __global__ void kModSmallPrimesSeg(
        const unsigned int* __restrict__ limbs,
        const int* __restrict__ segOff,     // nseg+1 offsets, little-endian index
        int nseg,
        const unsigned int* __restrict__ primes,
        int nprimes,
        unsigned int* __restrict__ partial)
{
    int i = blockIdx.y;
    int s = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= nprimes || s >= nseg) return;

    unsigned long long p = primes[i];
    unsigned long long r = 0;
    int hi = segOff[s + 1];
    for (int j = hi - 1; j >= segOff[s]; --j) {
        r = ((r << 32) | (unsigned long long)limbs[j]) % p;
    }
    partial[(size_t)i * nseg + s] = (unsigned int)r;
}

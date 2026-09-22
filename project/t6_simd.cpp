// Tier 6 — SIMD (NEON) string copy for the message payload.  (bonus flex)
// Compile: clang++ -std=c++20 -O2 -Wall -Wextra -pthread t6_simd.cpp -o t6 && ./t6
//
// Apple Silicon uses NEON (128-bit / 16-byte vectors), not AVX2. We copy the
// payload 16 bytes at a time with vld1q_u8 / vst1q_u8, plus a scalar tail.
//
// Honest goal: beat a NAIVE byte-by-byte loop convincingly, and measure how we
// compare to std::memcpy (which libc already vectorizes). Correctness first.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <vector>
#include <string>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#else
#error "This tier targets ARM NEON (Apple Silicon)."
#endif

// Baseline 1: naive scalar copy (what SIMD should beat).
static inline void scalar_copy(char *dst, const char *src, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = src[i];
}

// Our SIMD copy: 16 bytes per iteration via NEON, then a scalar tail.
static inline void neon_copy(char *dst, const char *src, std::size_t n)
{
    std::size_t i = 0;

    // ---------- TASK 1: the 16-byte NEON loop ----------
    // While at least 16 bytes remain (i + 16 <= n):
    //   - load 16 bytes from src+i into a uint8x16_t with vld1q_u8
    //     (cast to const uint8_t*)
    //   - store that vector to dst+i with vst1q_u8 (cast to uint8_t*)
    //   - advance i by 16
    // TODO
    while (i + 16 < n)
    {
        uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t *>(src) + i);
        vst1q_u8(reinterpret_cast<uint8_t *>(dst) + i, v);
        i += 16;
    }

    // scalar tail: copy the remaining (< 16) bytes
    for (; i < n; ++i)
        dst[i] = src[i];
}

// ---- timing helper: copy every message `reps` times, return ns per copy ----
template <class CopyFn>
static double bench_copy(CopyFn &&copy, const std::vector<std::string> &msgs,
                         std::vector<char> &scratch, int reps)
{
    volatile char sink = 0; // keep the compiler from optimizing the copy away
    auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; ++r)
    {
        for (const auto &m : msgs)
        {
            copy(scratch.data(), m.data(), m.size());
            sink ^= scratch[m.size() ? m.size() - 1 : 0];
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    (void)sink;
    double total = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return total / (double)(reps * msgs.size()); // ns per copy
}

// verify a copy function is byte-exact for a range of lengths
static bool verify(void (*copy)(char *, const char *, std::size_t))
{
    char src[128], dst[128];
    for (int len = 0; len <= 100; ++len)
    {
        for (int i = 0; i < len; ++i)
            src[i] = (char)('A' + (i % 26));
        std::memset(dst, 0, sizeof dst);
        copy(dst, src, (std::size_t)len);
        if (std::memcmp(dst, src, (std::size_t)len) != 0)
        {
            std::printf("  MISMATCH at len=%d\n", len);
            return false;
        }
    }
    return true;
}

int main()
{
    // correctness first
    std::printf("verify neon_copy   : %s\n", verify(neon_copy) ? "OK" : "FAIL");
    std::printf("verify scalar_copy : %s\n\n", verify(scalar_copy) ? "OK" : "FAIL");

    // build a realistic set of short log-message payloads
    const int N = 100'000;
    std::vector<std::string> msgs;
    msgs.reserve(N);
    for (int i = 0; i < N; ++i)
    {
        char m[64];
        int k = std::snprintf(m, sizeof m, "log line %d value=%d status=ok", i, i * 7);
        msgs.emplace_back(m, (std::size_t)k);
    }
    std::vector<char> scratch(128);
    const int reps = 200;

    double ns_scalar = bench_copy([](char *d, const char *s, std::size_t n)
                                  { scalar_copy(d, s, n); }, msgs, scratch, reps);
    double ns_neon = bench_copy([](char *d, const char *s, std::size_t n)
                                { neon_copy(d, s, n); }, msgs, scratch, reps);
    double ns_memcpy = bench_copy([](char *d, const char *s, std::size_t n)
                                  { std::memcpy(d, s, n); }, msgs, scratch, reps);

    std::printf("copy cost (ns per message, avg len ~%zu bytes):\n", msgs[0].size());
    std::printf("  scalar (byte loop) : %.2f ns\n", ns_scalar);
    std::printf("  neon (ours)        : %.2f ns   <- %.2fx vs scalar\n", ns_neon, ns_scalar / ns_neon);
    std::printf("  std::memcpy        : %.2f ns   (note: lambda blocks inlining; artifact)\n\n", ns_memcpy);

    // ---- size sweep: where does SIMD actually pay off? ----
    // Fixed-size copies of a single buffer, isolating the copy from per-message
    // overhead so NEON's advantage over scalar grows visibly with payload size.
    std::printf("size sweep (ns per copy, scalar vs neon):\n");
    std::printf("  %6s  %8s  %8s  %8s\n", "bytes", "scalar", "neon", "speedup");
    alignas(16) static char src[4096], dst[4096];
    for (int i = 0; i < 4096; ++i) src[i] = (char)(i & 0x7f);
    const int sweep_reps = 200000;
    for (std::size_t len : {16u, 32u, 64u, 128u, 256u, 1024u, 4096u}) {
        volatile char sink = 0;
        auto s0 = std::chrono::steady_clock::now();
        for (int r = 0; r < sweep_reps; ++r) { scalar_copy(dst, src, len); sink ^= dst[len-1]; }
        auto s1 = std::chrono::steady_clock::now();
        for (int r = 0; r < sweep_reps; ++r) { neon_copy(dst, src, len);   sink ^= dst[len-1]; }
        auto s2 = std::chrono::steady_clock::now();
        (void)sink;
        double sc = std::chrono::duration<double,std::nano>(s1-s0).count() / sweep_reps;
        double ne = std::chrono::duration<double,std::nano>(s2-s1).count() / sweep_reps;
        std::printf("  %6zu  %8.2f  %8.2f  %7.2fx\n", len, sc, ne, sc/ne);
    }
    return 0;
}

// false_sharing.cpp — isolates the cache-line-alignment win used in the engine.
// Compile: clang++ -std=c++20 -O2 -Wall -Wextra -pthread bench/false_sharing.cpp -o fs && ./fs
//
// Two threads each hammer their OWN atomic counter — logically independent, so it
// "should" scale perfectly. But if the two counters share a cache line, every
// increment on one invalidates the other core's copy and the line ping-pongs.
// Aligning each onto its own line (as the engine does for head_/tail_) fixes it.

#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>

static constexpr std::size_t kCacheLine = 128;   // Apple M3 (sysctl hw.cachelinesize)

template <bool Aligned>
struct Counters {
    alignas(Aligned ? kCacheLine : alignof(std::atomic<std::uint64_t>)) std::atomic<std::uint64_t> a{0};
    alignas(Aligned ? kCacheLine : alignof(std::atomic<std::uint64_t>)) std::atomic<std::uint64_t> b{0};
};

template <bool Aligned>
static double run(std::uint64_t iters) {
    Counters<Aligned> c;
    auto t0 = std::chrono::steady_clock::now();
    std::thread ta([&]{ for (std::uint64_t i = 0; i < iters; ++i) c.a.fetch_add(1, std::memory_order_relaxed); });
    std::thread tb([&]{ for (std::uint64_t i = 0; i < iters; ++i) c.b.fetch_add(1, std::memory_order_relaxed); });
    ta.join(); tb.join();
    auto t1 = std::chrono::steady_clock::now();
    return (2.0 * iters) / std::chrono::duration<double>(t1 - t0).count() / 1e6;   // M ops/sec
}

int main() {
    const std::uint64_t N = 200'000'000;
    double adj = run<false>(N);
    double ali = run<true>(N);
    std::printf("adjacent (same cache line) : %7.1f M ops/s\n", adj);
    std::printf("aligned  (separate lines)  : %7.1f M ops/s   <- %.2fx faster\n", ali, ali / adj);
    return 0;
}

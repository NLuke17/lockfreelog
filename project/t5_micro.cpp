// Tier 5 (part 3) — Isolated microbenchmarks for the two resume claims that the
// full-logger benchmark muddies:
//   (A) false sharing: cache-line alignment shown in ISOLATION (indices only).
//   (B) enqueue throughput: what a LOG() call costs the app thread (the headline).
//
// Compile: clang++ -std=c++20 -O2 -Wall -Wextra -pthread t5_micro.cpp -o t5micro && ./t5micro

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>
#include <array>
#include <vector>
#include <string>

static constexpr std::size_t CACHE_LINE = 128;   // Apple M3

// ============================================================================
// (A) FALSE SHARING, ISOLATED.
// Two threads each hammer their OWN atomic counter. Logically independent, so
// perfect scaling would be expected. But if the two counters share a cache line,
// every increment on one invalidates the other core's copy -> the line ping-pongs.
// We time adjacent (same line) vs aligned (separate lines).
// ============================================================================
template <bool Aligned>
struct Counters {
    alignas(Aligned ? CACHE_LINE : alignof(std::atomic<std::uint64_t>)) std::atomic<std::uint64_t> a{0};
    alignas(Aligned ? CACHE_LINE : alignof(std::atomic<std::uint64_t>)) std::atomic<std::uint64_t> b{0};
};

template <bool Aligned>
static double bench_false_sharing(std::uint64_t iters) {
    Counters<Aligned> c;
    auto t0 = std::chrono::steady_clock::now();
    std::thread ta([&]{ for (std::uint64_t i = 0; i < iters; ++i) c.a.fetch_add(1, std::memory_order_relaxed); });
    std::thread tb([&]{ for (std::uint64_t i = 0; i < iters; ++i) c.b.fetch_add(1, std::memory_order_relaxed); });
    ta.join(); tb.join();
    auto t1 = std::chrono::steady_clock::now();
    return (2.0 * iters) / std::chrono::duration<double>(t1 - t0).count() / 1e6;  // M ops/sec
}

// ============================================================================
// (B) ENQUEUE THROUGHPUT.
// The app-visible cost of a log call: producer pushes pre-formatted messages into
// the lock-free ring; a consumer drains with MINIMAL work (just advances tail) so
// it never bottlenecks the producer. This isolates push() cost = the enqueue rate
// a real app sustains while the background thread handles the actual sink.
// ============================================================================
static constexpr std::size_t CAPACITY    = 8192;
static constexpr std::size_t MAX_MSG_LEN = 64;
struct Slot { char data[MAX_MSG_LEN]; uint16_t len = 0; };

class Ring {
public:
    void start() { consumer_ = std::thread([this]{ drain(); }); }
    bool push(const char* msg, std::size_t len) {
        std::uint64_t h = head_.load(std::memory_order_relaxed);
        std::uint64_t t = tail_.load(std::memory_order_acquire);
        if (h - t == CAPACITY) return false;
        std::size_t idx = h & (CAPACITY - 1);
        std::memcpy(buf_[idx].data, msg, len);
        buf_[idx].len = static_cast<uint16_t>(len);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    void stop() { done_.store(true, std::memory_order_relaxed); consumer_.join(); }
    std::uint64_t sink() const { return sink_; }
private:
    void drain() {
        std::uint64_t t = tail_.load(std::memory_order_relaxed);
        for (;;) {
            std::uint64_t h = head_.load(std::memory_order_acquire);
            if (t == h) {
                if (done_.load(std::memory_order_relaxed)) { tail_.store(t, std::memory_order_release); return; }
                continue;
            }
            while (t != h) { sink_ += buf_[t & (CAPACITY - 1)].len; ++t; }  // minimal drain
            tail_.store(t, std::memory_order_release);
        }
    }
    alignas(CACHE_LINE) std::atomic<std::uint64_t> head_{0};
    alignas(CACHE_LINE) std::atomic<std::uint64_t> tail_{0};
    std::array<Slot, CAPACITY> buf_{};
    std::atomic<bool> done_{false};
    std::uint64_t sink_ = 0;
    std::thread consumer_;
};

int main() {
    std::printf("=== (A) false sharing, isolated (M ops/sec) ===\n");
    const std::uint64_t ITERS = 200'000'000;
    double adj = bench_false_sharing<false>(ITERS);
    double ali = bench_false_sharing<true>(ITERS);
    std::printf("adjacent (same cache line) : %7.1f M/s\n", adj);
    std::printf("aligned  (separate lines)  : %7.1f M/s   <- %.2fx faster\n\n", ali, ali / adj);

    std::printf("=== (B) enqueue throughput of the lock-free ring (M msgs/sec) ===\n");
    const int N = 10'000'000;
    std::vector<std::string> msgs; msgs.reserve(N);
    for (int i = 0; i < N; ++i) {
        char m[48]; int k = std::snprintf(m, sizeof m, "log line %d value=%d", i, i * 7);
        msgs.emplace_back(m, (std::size_t)k);
    }
    Ring ring; ring.start();
    auto t0 = std::chrono::steady_clock::now();
    for (const auto& s : msgs) while (!ring.push(s.data(), s.size())) {}
    auto t1 = std::chrono::steady_clock::now();
    ring.stop();
    double rate = N / std::chrono::duration<double>(t1 - t0).count() / 1e6;
    std::printf("enqueue throughput         : %7.1f M msgs/sec\n", rate);
    std::printf("(sink checksum = %llu, keeps the drain from being optimized away)\n",
                (unsigned long long)ring.sink());
    return 0;
}

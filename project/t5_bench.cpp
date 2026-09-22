// Tier 5 — Cache-line alignment + benchmark harness.
// Compile: clang++ -std=c++20 -O2 -Wall -Wextra -pthread t5_bench.cpp -o t5 && ./t5
//
// Two jobs:
//  (1) Prove the false-sharing fix: benchmark head_/tail_ ADJACENT (same cache
//      line) vs ALIGNED (each on its own 128-byte line on Apple Silicon).
//  (2) Benchmark our lock-free logger's push throughput against printf / iostream
//      (and fmt / spdlog — see t5_libs.cpp once those are installed).
//
// Honest measurement rules:
//  - messages are PRE-FORMATTED outside the timed region (we measure the queue,
//    not snprintf). T4 will make formatting itself cheap.
//  - the consumer keeps up (drains via a cheap checksum) so the push rate is
//    SUSTAINED, not just "fits in the buffer once".

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <string>

// M3 has 128-byte cache lines (verified: sysctl hw.cachelinesize == 128).
static constexpr std::size_t CACHE_LINE = 128;
static constexpr std::size_t CAPACITY = 8192;
static constexpr std::size_t MAX_MSG_LEN = 64;
static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be a power of two");

struct Slot
{
    char data[MAX_MSG_LEN];
    uint16_t len = 0;
};

// A template flag lets us compile the SAME logic twice: once with the indices
// adjacent (false sharing) and once aligned onto separate cache lines.
template <bool Aligned>
class Ring
{
public:
    void start()
    {
        consumer_ = std::thread([this]
                                { consume_loop(); });
    }

    bool push(const char *msg, std::size_t len)
    {
        std::uint64_t h = head_.load(std::memory_order_relaxed);
        std::uint64_t t = tail_.load(std::memory_order_acquire);
        if (h - t == CAPACITY)
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::size_t idx = h & (CAPACITY - 1);
        std::memcpy(buf_[idx].data, msg, len);
        buf_[idx].len = static_cast<uint16_t>(len);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    void stop()
    {
        done_.store(true, std::memory_order_relaxed);
        consumer_.join();
    }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t flushed() const { return flushed_; }
    std::uint64_t checksum() const { return checksum_; }

private:
    void consume_loop()
    {
        std::uint64_t t = tail_.load(std::memory_order_relaxed);
        for (;;)
        {
            std::uint64_t h = head_.load(std::memory_order_acquire);
            if (t == h)
            {
                if (done_.load(std::memory_order_relaxed))
                {
                    tail_.store(t, std::memory_order_release);
                    return;
                }
                continue; // pure spin: we want max drain rate for the benchmark
            }
            while (t != h)
            {
                std::size_t idx = t & (CAPACITY - 1);
                for (std::uint16_t i = 0; i < buf_[idx].len; ++i)
                    checksum_ = checksum_ * 1315423911u + (unsigned char)buf_[idx].data[i];
                ++flushed_;
                ++t;
            }
            tail_.store(t, std::memory_order_release);
        }
    }

    // ---------- TASK 1: the two layouts ----------
    // When Aligned == true, head_ and tail_ must each sit on their OWN cache line
    // so the producer's writes to head_ never invalidate the consumer's line for
    // tail_ (and vice versa). When Aligned == false, leave them adjacent (the bug).
    //
    // Use a conditional alignment. One clean way:
    //   alignas(Aligned ? CACHE_LINE : alignof(std::atomic<std::uint64_t>))
    // on BOTH head_ and tail_. That expands to 128-byte alignment when Aligned,
    // and natural (8-byte) alignment otherwise — putting them back on one line.
    //
    // TODO: add the alignas(...) specifier to each of the two atomics below.
    alignas(Aligned ? CACHE_LINE : alignof(std::atomic<std::uint64_t>))
        std::atomic<std::uint64_t> head_{0};
    alignas(Aligned ? CACHE_LINE : alignof(std::atomic<std::uint64_t>))
        std::atomic<std::uint64_t> tail_{0};

    std::array<Slot, CAPACITY> buf_{};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool> done_{false};
    std::uint64_t flushed_ = 0;
    std::uint64_t checksum_ = 0;
    std::thread consumer_;
};

// Pre-format N messages so the timed loop measures the QUEUE, not formatting.
static std::vector<std::string> make_messages(int n)
{
    std::vector<std::string> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        char m[48];
        int k = std::snprintf(m, sizeof m, "log line %d value=%d", i, i * 7);
        v.emplace_back(m, (std::size_t)k);
    }
    return v;
}

template <bool Aligned>
static double bench_ring(const std::vector<std::string> &msgs)
{
    Ring<Aligned> ring;
    ring.start();
    auto t0 = std::chrono::steady_clock::now();
    for (const auto &s : msgs)
    {
        while (!ring.push(s.data(), s.size()))
        { /* full: retry so nothing is dropped */
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    ring.stop();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    return msgs.size() / secs / 1e6; // M msgs/sec
}

// Baselines: what "just log it inline" costs with the standard tools.
static double bench_printf(const std::vector<std::string> &msgs)
{
    FILE *dev = std::fopen("/dev/null", "w");
    auto t0 = std::chrono::steady_clock::now();
    for (const auto &s : msgs)
        std::fprintf(dev, "%s\n", s.c_str());
    auto t1 = std::chrono::steady_clock::now();
    std::fclose(dev);
    return msgs.size() / std::chrono::duration<double>(t1 - t0).count() / 1e6;
}

int main()
{
    const int N = 5'000'000;
    auto msgs = make_messages(N);

    std::printf("=== Tier 5 benchmark (%d messages, msgs/sec in millions) ===\n", N);
    std::printf("cache line = %zu bytes (Apple M3)\n\n", CACHE_LINE);

    double adj = bench_ring<false>(msgs);
    double ali = bench_ring<true>(msgs);
    double pf = bench_printf(msgs);

    std::printf("ring (adjacent / false sharing) : %6.1f M/s\n", adj);
    std::printf("ring (aligned  / fixed)         : %6.1f M/s   <- %.2fx faster\n", ali, ali / adj);
    std::printf("fprintf -> /dev/null            : %6.1f M/s\n", pf);
    std::printf("\nspeedup of aligned lock-free vs fprintf: %.1fx\n", ali / pf);
    return 0;
}

// Tier 3 — LOCK-FREE SPSC ring buffer logger.  ⭐ the headline.
// Compile: clang++ -std=c++20 -O2 -Wall -Wextra -pthread t3_spsc.cpp -o t3 && ./t3
//
// No mutex. No condvar. head_/tail_ are std::atomic<uint64_t>, coordinated purely
// by the acquire/release handoff from L3. The producer's push() is WAIT-FREE:
// a fixed, bounded sequence of instructions that never blocks on the consumer.
//
// SPSC invariant (what makes this safe without CAS):
//   - producer is the ONLY writer of head_   (consumer only reads it)
//   - consumer is the ONLY writer of tail_   (producer only reads it)
//   => each side loads its OWN index relaxed; ordering is paid only on the
//      cross-thread reads via two acquire/release pairs.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <array>
#include <atomic>
#include <thread>
#include <chrono>

static constexpr std::size_t CAPACITY = 1024; // power of two
static constexpr std::size_t MAX_MSG_LEN = 64;
static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be a power of two");

struct Slot
{
    char data[MAX_MSG_LEN];
    uint16_t len = 0;
};

class SpscLogger
{
public:
    void start()
    {
        consumer_ = std::thread([this]
                                { consume_loop(); });
    }

    // PRODUCER hot path. Wait-free: no lock, no blocking. Returns false if full.
    bool push(const char *msg, std::size_t len)
    {
        if (len > MAX_MSG_LEN)
            len = MAX_MSG_LEN;

        // ---------- TASK 1: read the indices ----------
        // - load head_ into h. We are the ONLY writer of head_, so use RELAXED.
        // - load tail_ into t. The consumer writes tail_, and we must see the slots
        //   it has freed before reusing them, so use ACQUIRE.
        // TODO
        std::uint64_t h = head_.load(std::memory_order_relaxed);
        std::uint64_t t = tail_.load(std::memory_order_acquire);

        // full check: buffer holds (h - t) messages
        if (h - t == CAPACITY)
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // plain writes into the slot (published by the release store below)
        std::size_t idx = h & (CAPACITY - 1);
        std::memcpy(buf_[idx].data, msg, len);
        buf_[idx].len = static_cast<uint16_t>(len);

        // ---------- TASK 2: publish ----------
        // Store h+1 into head_ with RELEASE. This publishes the slot writes above:
        // when the consumer's ACQUIRE-load of head_ sees this value, it is
        // guaranteed to see the bytes we just wrote.
        // TODO
        head_.fetch_add(1, std::memory_order_release);

        return true;
    }
    bool push(const char *msg) { return push(msg, std::strlen(msg)); }

    void stop()
    {
        done_.store(true, std::memory_order_relaxed); // tell consumer to finish
        consumer_.join();
    }

    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t flushed() const { return flushed_; }
    std::uint64_t checksum() const { return checksum_; }

private:
    // CONSUMER: spin-drain the ring until done_ AND empty. No lock.
    void consume_loop()
    {
        std::uint64_t t = tail_.load(std::memory_order_relaxed); // we own tail_
        for (;;)
        {
            // ---------- TASK 3: read head with ACQUIRE ----------
            // Load head_ into h with ACQUIRE so we see the producer's slot bytes.
            // TODO
            std::uint64_t h = head_.load(std::memory_order_acquire);

            if (t == h)
            {
                // nothing to drain right now
                if (done_.load(std::memory_order_relaxed))
                {
                    // publish our final tail_ position and exit
                    tail_.store(t, std::memory_order_release);
                    return;
                }
                std::this_thread::yield(); // spin politely; T5 will revisit this
                continue;
            }

            // drain everything currently available [t, h)
            while (t != h)
            {
                std::size_t idx = t & (CAPACITY - 1);
                // "consume": fold the bytes into a checksum (stands in for real I/O,
                // but fast enough to actually keep up so we can measure push rate)
                for (std::uint16_t i = 0; i < buf_[idx].len; ++i)
                    checksum_ = checksum_ * 1315423911u + (unsigned char)buf_[idx].data[i]; // what is this?
                ++flushed_;
                ++t;
            }

            // ---------- TASK 4: publish freed slots ----------
            // Store t into tail_ with RELEASE so the producer can safely reuse the
            // slots we just finished reading.
            // TODO
            tail_.store(t, std::memory_order_release);
        }
    }

    std::array<Slot, CAPACITY> buf_{};
    std::atomic<std::uint64_t> head_{0}; // producer writes, consumer reads
    std::atomic<std::uint64_t> tail_{0}; // consumer writes, producer reads
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool> done_{false};

    // consumer-only (no sharing): plain, not atomic
    std::uint64_t flushed_ = 0;
    std::uint64_t checksum_ = 0;

    std::thread consumer_;
};

// reference checksum computed single-threaded, to verify the lock-free path
static std::uint64_t ref_checksum(const char *s, std::size_t len, std::uint64_t cs)
{
    if (len > MAX_MSG_LEN)
        len = MAX_MSG_LEN;
    for (std::size_t i = 0; i < len; ++i)
        cs = cs * 1315423911u + (unsigned char)s[i];
    return cs;
}

int main()
{
    SpscLogger log;
    log.start();

    const int N = 5'000'000; // 5M messages
    std::uint64_t expected_cs = 0;
    std::uint64_t accepted = 0;

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i)
    {
        char m[48];
        int n = std::snprintf(m, sizeof m, "log line %d", i);
        // NOTE: with drops, the checksum can't be compared 1:1 across all N, so we
        // size things to keep up. If drops==0, expected_cs must equal checksum().
        if (log.push(m, (std::size_t)n))
        {
            expected_cs = ref_checksum(m, (std::size_t)n, expected_cs);
            ++accepted;
        }
    }
    auto t1 = std::chrono::steady_clock::now();

    log.stop();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    double rate = N / secs;

    std::fprintf(stderr, "-- tier 3 (lock-free SPSC) --\n");
    std::fprintf(stderr, "messages         = %d\n", N);
    std::fprintf(stderr, "accepted         = %llu\n", (unsigned long long)accepted);
    std::fprintf(stderr, "dropped          = %llu\n", (unsigned long long)log.dropped());
    std::fprintf(stderr, "flushed          = %llu\n", (unsigned long long)log.flushed());
    std::fprintf(stderr, "push throughput  = %.1f M msgs/sec\n", rate / 1e6);
    std::fprintf(stderr, "accepted==flushed? %s\n", (accepted == log.flushed()) ? "yes" : "NO");
    if (log.dropped() == 0)
        std::fprintf(stderr, "checksum match?    %s\n",
                     (expected_cs == log.checksum()) ? "yes" : "NO (CORRUPTION!)");
    else
        std::fprintf(stderr, "checksum: skipped (had drops; integrity shown by accepted==flushed)\n");
    return 0;
}

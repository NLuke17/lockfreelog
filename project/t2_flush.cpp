// Tier 2 — Background flush thread (MUTEX version). Correct before fast.
// Compile: clang++ -std=c++20 -O2 -Wall -Wextra -pthread t2_flush.cpp -o t2 && ./t2
//
// Architecture: the producer (app thread) only push()es. A background consumer
// thread drains the ring to stdout on its own, absorbing the I/O latency off the
// hot path. Protected by a std::mutex + condition_variable (your L2 pattern).
//
// This is Tier 3's design MINUS lock-free. In T3 we replace the mutex with atomic
// head/tail + acquire/release and MEASURE the speedup. Get correctness nailed here.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <array>
#include <thread>
#include <mutex>
#include <condition_variable>

static constexpr std::size_t CAPACITY = 1024; // power of two
static constexpr std::size_t MAX_MSG_LEN = 64;
static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be a power of two");

struct Slot
{
    char data[MAX_MSG_LEN];
    uint16_t len = 0;
};

class Logger
{
public:
    // Start the background flush thread.
    void start()
    {
        flusher_ = std::thread([this]
                               { flush_loop(); });
    }

    // Producer API. Returns false if the buffer was full (message dropped).
    bool push(const char *msg, std::size_t len)
    {
        if (len > MAX_MSG_LEN)
            len = MAX_MSG_LEN;

        // ---------- TASK 1: push under the lock ----------
        // - take a unique_lock (we'll notify a condvar, and keep it simple/correct)
        // - full check: if (head_ - tail_ == CAPACITY) { ++dropped_; return false; }
        // - write the slot: idx = head_ & (CAPACITY-1); memcpy data; set len
        // - ++head_
        // - AFTER updating state, notify the flush thread there's work (data_ready_)
        //   (you may unlock before notifying — that's fine and slightly faster)
        // TODO
        std::unique_lock<std::mutex> lk(m_);
        if (head_ - tail_ == CAPACITY)
        {
            ++dropped_;
            return false;
        }
        std::uint16_t idx = head_ & (CAPACITY - 1);
        std::memcpy(buf_[idx].data, msg, len);
        std::memcpy(&buf_[idx].len, &len, sizeof(std::uint16_t));
        ++head_;
        data_ready_.notify_one();

        return true;
    }
    bool push(const char *msg) { return push(msg, std::strlen(msg)); }

    // Signal shutdown and join the flush thread (drains whatever remains first).
    void stop()
    {
        // ---------- TASK 3: shutdown ----------
        // - under the lock, set done_ = true
        // - notify the flush thread (it may be asleep waiting for data)
        // - join flusher_
        // Order in main() matters: call stop() only AFTER all pushes are done.
        // TODO
        {
            std::unique_lock<std::mutex> lk(m_);
            done_ = true;
        }
        data_ready_.notify_all();
        flusher_.join();
    }

    std::uint64_t dropped() const { return dropped_; }
    std::uint64_t flushed() const { return flushed_; }

private:
    // Background thread body: drain the ring to stdout until done_ AND empty.
    void flush_loop()
    {
        for (;;)
        {
            std::unique_lock<std::mutex> lk(m_);

            // ---------- TASK 2: wait, then drain a batch ----------
            // - wait on data_ready_ until (head_ != tail_) OR done_
            //     (i.e. there is something to flush, or we're shutting down)
            // - if (head_ == tail_ && done_) return;   // nothing left and told to stop
            // - drain everything currently available: while (tail_ != head_) { ... }
            //     for each: idx = tail_ & (CAPACITY-1); fwrite data/len; putchar('\n');
            //               ++tail_; ++flushed_;
            //   NOTE: doing I/O while holding the lock is SLOW and blocks the producer.
            //   That inefficiency is EXACTLY what Tier 3 fixes. For now: correct first.
            // TODO
            data_ready_.wait(lk, [&]
                             { return head_ != tail_ || done_; });
            if (head_ == tail_ && done_)
            {
                return;
            }
            while (tail_ < head_)
            {
                std::uint16_t idx = tail_ & (CAPACITY - 1);
                fwrite(buf_[idx].data, 1, buf_[idx].len, stdout);
                putchar('\n');
                ++tail_;
                ++flushed_;
            }
            (void)lk;
        }
    }

    std::array<Slot, CAPACITY> buf_{};
    std::uint64_t head_ = 0; // total written (producer writes)
    std::uint64_t tail_ = 0; // total read    (consumer writes)
    std::uint64_t dropped_ = 0;
    std::uint64_t flushed_ = 0;

    std::mutex m_;
    std::condition_variable data_ready_;
    bool done_ = false;
    std::thread flusher_;
};

int main()
{
    Logger log;
    log.start();

    const int N = 100'000;
    int pushed = 0;
    for (int i = 0; i < N; ++i)
    {
        char m[48];
        int n = std::snprintf(m, sizeof m, "log line %d", i);
        if (log.push(m, (std::size_t)n))
            ++pushed;
    }

    log.stop(); // AFTER all pushes: drains the rest, then joins

    // stderr so it isn't interleaved into the flushed stdout stream
    std::fprintf(stderr, "\n-- tier 2 summary --\n");
    std::fprintf(stderr, "pushed  = %d\n", pushed);
    std::fprintf(stderr, "flushed = %llu\n", (unsigned long long)log.flushed());
    std::fprintf(stderr, "dropped = %llu\n", (unsigned long long)log.dropped());
    // Correct invariants: every accepted push is flushed, and every attempt is
    // accounted for (accepted + dropped == N). 'pushed' already excludes drops.
    std::fprintf(stderr, "flushed == pushed ?              %s\n",
                 ((int)log.flushed() == pushed) ? "yes" : "NO");
    std::fprintf(stderr, "pushed + dropped == N ?          %s\n",
                 (pushed + (int)log.dropped() == N) ? "yes" : "NO");
    return 0;
}

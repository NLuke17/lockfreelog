// Tier 1 — Zero-alloc, single-threaded ring buffer logger.
// Compile: clang++ -std=c++20 -O2 -Wall -Wextra -pthread t1_ring.cpp -o t1 && ./t1
//
// Proves: the data structure. A fixed, preallocated ring of message slots.
// NO heap allocation in the hot path (push). Wrap-around via a power-of-two mask.
//
// Design (carries all the way to the lock-free Tier 3):
//   head_ = total messages ever WRITTEN   (monotonically increasing)
//   tail_ = total messages ever READ      (monotonically increasing)
//   count in buffer = head_ - tail_        (0 <= count <= CAPACITY)
//   slot for the i-th message = i & (CAPACITY-1)   (cheap mask, needs power-of-2)

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <array>

// ---- fixed compile-time sizes: everything is preallocated, no new/malloc ----
static constexpr std::size_t CAPACITY = 1024; // MUST be a power of two
static constexpr std::size_t MAX_MSG_LEN = 64;
static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be a power of two");

struct Slot
{
    char data[MAX_MSG_LEN];
    uint16_t len = 0;
};

class RingLogger
{
public:
    RingLogger() = default;

    // Returns true if the message was stored, false if the buffer was full (dropped).
    // HOT PATH: must not allocate. Just copy bytes into the current head slot.
    bool push(const char *msg, std::size_t len)
    {
        // ---------- TASK 1: full check ----------
        // count in buffer = head_ - tail_. If that equals CAPACITY, we're full:
        // return false (drop the message). Do NOT block, do NOT allocate.
        // TODO
        if (head_ - tail_ == CAPACITY)
        { // does tail need to acquired atomically
            return false;
        }

        // clamp overly long messages to the slot size (defensive; no realloc)
        if (len > MAX_MSG_LEN)
            len = MAX_MSG_LEN;

        // ---------- TASK 2: write into the head slot ----------
        // - compute the slot index: head_ & (CAPACITY - 1)
        // - std::memcpy the msg bytes into that slot's data
        // - set that slot's len
        // TODO
        std::memcpy(buf_[head_ & (CAPACITY - 1)].data, msg, len);
        std::memcpy(&buf_[head_ & (CAPACITY - 1)].len, &len, sizeof(uint16_t));

        // ---------- TASK 3: advance head ----------
        // We've published one more message. Bump head_ by 1.
        // TODO
        head_++;

        return true;
    }

    // Convenience overload for C-string literals in the demo.
    bool push(const char *msg) { return push(msg, std::strlen(msg)); }

    // Drain everything currently buffered to stdout, oldest first.
    // Single-threaded here; in Tier 2 this moves to a background thread.
    void drain()
    {
        // ---------- TASK 4: drain from tail_ up to head_ ----------
        // While tail_ < head_:
        //   - slot index = tail_ & (CAPACITY - 1)
        //   - write exactly slot.len bytes of slot.data to stdout, then a '\n'
        //     (use fwrite(slot.data, 1, slot.len, stdout); putchar('\n');)
        //   - advance tail_ by 1
        // TODO
        while (tail_ < head_)
        {
            fwrite(buf_[tail_ & (CAPACITY - 1)].data, 1, buf_[tail_ & (CAPACITY - 1)].len, stdout);
            putchar('\n');
            tail_++;
        }
    }

    std::size_t buffered() const { return head_ - tail_; }
    std::uint64_t dropped() const { return dropped_; }

private:
    std::array<Slot, CAPACITY> buf_{}; // the whole buffer, preallocated inline
    std::uint64_t head_ = 0;           // total written
    std::uint64_t tail_ = 0;           // total read
    std::uint64_t dropped_ = 0;        // messages dropped because buffer was full
};

// ------------------------------- demo / self-check ---------------------------
int main()
{
    RingLogger log;

    // 1) basic round-trip: push a few, drain, eyeball the order
    log.push("hello");
    log.push("world");
    log.push("lock-free logging, tier 1");
    std::printf("-- drain 1 (expect 3 lines in order) --\n");
    log.drain();
    std::printf("buffered after drain = %zu (expect 0)\n\n", log.buffered());

    // 2) fill exactly to capacity, then one more must be dropped
    RingLogger log2;
    std::size_t stored = 0;
    for (std::size_t i = 0; i < CAPACITY + 50; ++i)
    {
        char m[32];
        int n = std::snprintf(m, sizeof m, "msg-%zu", i);
        if (log2.push(m, (std::size_t)n))
            ++stored;
    }
    std::printf("-- capacity test --\n");
    std::printf("stored   = %zu (expect %zu)\n", stored, CAPACITY);
    std::printf("buffered = %zu (expect %zu)\n", log2.buffered(), CAPACITY);
    // (we pushed CAPACITY+50; first CAPACITY fit, next 50 dropped)

    return 0;
}

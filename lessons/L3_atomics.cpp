// L3 — Atomics & the memory model.
// Compile: clang++ -std=c++20 -O2 -Wall -Wextra -pthread L3_atomics.cpp -o L3 && ./L3
//
// Ex1: fix the L1 race with std::atomic (works at -O2 now).
// Ex2: the acquire/release handoff — the exact idiom the ring buffer uses.
// Ex3: SEE the CPU reorder memory (relaxed vs seq_cst), the way you saw the L1 race.

#include <iostream>
#include <thread>
#include <atomic>
#include <barrier>
#include <vector>

// =====================================================================
// EXERCISE 1 — Atomic counter (the real fix for L1)
// Two threads each add 1, one million times, to a SHARED atomic counter.
// Unlike L1 this must equal 2,000,000 EVERY run, even at -O2.
// =====================================================================
void bump_atomic(std::atomic<long long> &counter, int times)
{
    for (int i = 0; i < times; ++i)
    {
        // ---------- TASK 1 ----------
        // Atomically add 1 to counter. Use relaxed ordering — explain to yourself
        // WHY relaxed is enough here (hint: nothing else depends on WHEN each
        // increment becomes visible; we only read the total after both join()).
        // TODO
        counter.fetch_add(1, std::memory_order_relaxed);
        // we can get away with relaxed as it doesn't matter the order of threads in which we add 1 to counter
        (void)i;
    }
}

void exercise1()
{
    std::atomic<long long> counter{0};
    std::thread a(bump_atomic, std::ref(counter), 1'000'000);
    std::thread b(bump_atomic, std::ref(counter), 1'000'000);
    a.join();
    b.join();
    std::cout << "[Ex1] counter = " << counter.load()
              << " (expected 2000000)  "
              << (counter.load() == 2'000'000 ? "OK" : "FAIL") << "\n";
}

// =====================================================================
// EXERCISE 2 — The acquire/release handoff (ring-buffer core)
// Producer writes a payload, then PUBLISHES it by storing a flag with release.
// Consumer waits for the flag with acquire, then reads the payload.
// If the handoff is correct, the consumer ALWAYS sees the full payload.
// =====================================================================
void exercise2()
{
    int payload = 0; // plain int — guarded only by the flag
    std::atomic<bool> ready{false};

    std::thread consumer([&]
                         {
                             // ---------- TASK 2a ----------
                             // Spin until ready is true, using an ACQUIRE load.
                             // Then read payload and check it == 123.
                             // TODO
                             // (after the loop) print: "[Ex2] consumer saw payload = " << payload
                             // and whether it equals 123.
                             while (!ready.load(std::memory_order_acquire))
                             {
                             }
                             std::cout << "[Ex2] consumer saw payload = " << payload << " 123 :: " << (payload == 123) << std::endl;
                             // does using cout make this no longer lock free?
                         });

    // ---------- TASK 2b ----------
    // Producer side (on this thread):
    //   1. write payload = 123    (plain write, BEFORE the publish)
    //   2. store ready = true with RELEASE ordering
    // TODO
    payload = 123;
    ready.store(true, std::memory_order_release);

    consumer.join();
}

// =====================================================================
// EXERCISE 3 — SEE the reordering (store-buffer litmus test)
// Two threads. T1: x=1 then read y.  T2: y=1 then read x.
// Under a single global order (seq_cst) it is IMPOSSIBLE for BOTH to read 0.
// Under relaxed, the CPU's store buffer can delay both stores, so BOTH loads
// can read 0. We run many trials and COUNT how often (r1==0 && r2==0) happens.
//
// This exercise is mostly written for you. Your job (TASK 3): set ORDER below
// to memory_order_relaxed, run it, note the count. Then change it to
// memory_order_seq_cst, rebuild, run again, and compare. Paste BOTH counts.
// =====================================================================
constexpr std::memory_order ORDER = std::memory_order_seq_cst; // <-- TASK 3: flip this
// 193592 / 200000 trials on relaxed

void exercise3()
{
    const int TRIALS = 200'000;
    int both_zero = 0;

    std::atomic<int> x{0}, y{0};
    int r1 = 0, r2 = 0;

    // Two persistent worker threads + main, synchronized each trial by a barrier
    // used at two points: (A) release both workers to run the trial together,
    // (B) wait for both to finish their store+load before main checks/resets.
    std::barrier sync(3);

    std::thread t1([&]
                   {
        for (int t = 0; t < TRIALS; ++t) {
            sync.arrive_and_wait();      // (A) start together
            x.store(1, ORDER);
            r1 = y.load(ORDER);
            sync.arrive_and_wait();      // (B) done
        } });
    std::thread t2([&]
                   {
        for (int t = 0; t < TRIALS; ++t) {
            sync.arrive_and_wait();      // (A)
            y.store(1, ORDER);
            r2 = x.load(ORDER);
            sync.arrive_and_wait();      // (B)
        } });

    for (int t = 0; t < TRIALS; ++t)
    {
        x.store(0, std::memory_order_relaxed); // reset BEFORE releasing workers
        y.store(0, std::memory_order_relaxed);
        sync.arrive_and_wait(); // (A) release the two workers
        sync.arrive_and_wait(); // (B) wait for both loads to complete
        if (r1 == 0 && r2 == 0)
            ++both_zero;
    }
    t1.join();
    t2.join();

    std::cout << "[Ex3] ORDER=" << (ORDER == std::memory_order_relaxed ? "relaxed" : "seq_cst")
              << "  both-read-0 happened " << both_zero << " / " << TRIALS << " trials\n";
}

int main()
{
    exercise1();
    exercise2();
    exercise3();
    return 0;
}

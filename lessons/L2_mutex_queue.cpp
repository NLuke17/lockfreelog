// L2 — Mutex + condition_variable: a correct, locked, bounded producer/consumer queue.
// Compile: clang++ -std=c++20 -O2 -Wall -Wextra -pthread L2_mutex_queue.cpp -o L2 && ./L2
//
// This is Tier 2 of the logger minus "lock-free": a fixed-capacity queue where a
// producer thread pushes ints and a consumer thread drains them. Correct first.
//
// Goals: lock_guard vs unique_lock, condition_variable with a PREDICATE loop,
// a two-condition bounded buffer, and a clean shutdown that never hangs.

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <optional>

class BoundedQueue
{
public:
    explicit BoundedQueue(std::size_t capacity) : cap_(capacity) {}

    // ---------- TASK 1: push (blocks while full) ----------
    // - take a unique_lock on m_
    // - wait on not_full_ until (q_.size() < cap_) OR done_ is true
    // - if done_ is true, just return (we're shutting down, drop the value)
    // - otherwise q_.push(value), then notify ONE waiter on not_empty_
    void push(int value)
    {
        // TODO
        std::unique_lock<std::mutex> lock(m_);
        not_full_.wait(lock, [&]
                       { return q_.size() < cap_ || done_; });
        if (done_)
        {
            std::cout << "we're shutting down, drop the value\n";
        }
        else
        {
            q_.push(value);
            not_empty_.notify_one();
        }
    }

    // ---------- TASK 2: pop (blocks while empty) ----------
    // Return std::optional<int>: a value when one is available, or std::nullopt
    // when the queue is empty AND done_ (the shutdown signal to the consumer).
    // - take a unique_lock on m_
    // - wait on not_empty_ until (!q_.empty()) OR done_
    // - if q_ is empty here, it must be because done_ -> return std::nullopt
    // - otherwise pop the front value, notify ONE waiter on not_full_, return it
    std::optional<int> pop()
    {
        // TODO
        std::unique_lock<std::mutex> lock(m_);
        not_empty_.wait(lock, [&]()
                        { return !q_.empty() || done_; });
        if (!q_.empty())
        {
            int v = q_.front();
            q_.pop();
            not_full_.notify_one();
            return v;
        }
        return std::nullopt;
    }

    // ---------- TASK 3: shutdown ----------
    // - lock m_, set done_ = true, unlock (or use a scoped block)
    // - notify_all() on BOTH condvars so nobody sleeps forever
    void stop()
    {
        // TODO
        {
            std::lock_guard<std::mutex> lk(m_);
            done_ = true;
        }
        not_full_.notify_all();
        not_empty_.notify_all();
    }

private:
    std::size_t cap_;
    std::queue<int> q_;
    bool done_ = false;
    std::mutex m_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
};

int main()
{
    BoundedQueue q(8); // small capacity on purpose: producer WILL block
    const int N = 100'000;
    long long sum = 0; // consumer accumulates; we check it at the end

    // ----- consumer thread: pop until pop() returns nullopt -----
    // TODO (TASK 4): start a thread that loops:
    //   while (auto v = q.pop()) sum += *v;
    // (pop() returns nullopt only after stop() AND the queue has drained)
    std::thread consumer([&q](long long &sum)
                         {
        while (auto v = q.pop()) {
            sum += *v;
        } }, std::ref(sum));

    // ----- producer: push N values 0..N-1 on THIS thread -----
    // TODO (TASK 5): push 0,1,2,...,N-1 into q.
    std::thread producer([&]()
                         {
        for (int i = 0; i < N; ++i) {
            q.push(i);
        } });

    // ----- shutdown: tell the queue we're done, then join -----
    // TODO (TASK 6): call q.stop(), then consumer.join().
    // Order matters: stop() AFTER all pushes so the consumer drains everything.
    producer.join();
    q.stop();
    consumer.join();

    // Expected: sum of 0..N-1 = N*(N-1)/2
    long long expected = (long long)N * (N - 1) / 2;
    std::cout << "sum      = " << sum << "\n";
    std::cout << "expected = " << expected << "\n";
    std::cout << (sum == expected ? "OK\n" : "MISMATCH\n");
    return 0;
}

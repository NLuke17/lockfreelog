// L2b — More mutex + condition_variable practice (two new patterns).
// Compile: clang++ -std=c++20 -O2 -Wall -Wextra -pthread L2b_practice.cpp -o L2b && ./L2b
//
// Exercise 1: turn-taking ping-pong  (predicate on a plain state variable)
// Exercise 2: a REUSABLE barrier      (why you need a generation counter)

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <string>

// =====================================================================
// EXERCISE 1 — Ping-Pong
// Two threads alternate printing. 'turn' says whose go it is:
//   turn == 0 -> ping's turn, turn == 1 -> pong's turn.
// Each thread does ROUNDS iterations. Output must strictly alternate:
//   ping pong ping pong ...  (2*ROUNDS lines total)
// =====================================================================
class PingPong
{
public:
    // who: 0 = "ping", 1 = "pong". Runs `rounds` times.
    void run(int who, int rounds, const std::string &word)
    {
        for (int i = 0; i < rounds; ++i)
        {
            // ---------- TASK 1 ----------
            // - take a unique_lock on m_
            // - wait on cv_ until it is THIS thread's turn (turn_ == who)
            // - print: word << " "     (no newline; we print one line at the end)
            // - flip the turn to the OTHER thread (turn_ = 1 - who)
            // - notify the other thread
            // TODO
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&]
                     { return turn_ == who; });
            std::cout << word << " ";
            turn_ = 1 - who;
            cv_.notify_one();
            (void)i;
            (void)word;
        }
    }

private:
    int turn_ = 0; // starts on ping
    std::mutex m_;
    std::condition_variable cv_;
};

// =====================================================================
// EXERCISE 2 — Reusable barrier
// N threads repeatedly do: local work, then arrive_and_wait().
// After every thread has called arrive_and_wait() for a round, they all
// release together and go to the next round. Must survive MANY rounds.
// =====================================================================
class Barrier
{
public:
    explicit Barrier(int n) : total_(n), count_(n) {}

    void arrive_and_wait()
    {
        // ---------- TASK 2 ----------
        // - take a unique_lock on m_
        // - snapshot the current generation: int g = gen_;
        // - decrement count_
        // - if count_ == 0  (I'm the last to arrive):
        //       * reset count_ = total_   (for the next round)
        //       * bump the generation: ++gen_
        //       * notify_all   (release everyone)
        //   else (not last):
        //       * wait on cv_ until gen_ != g   (i.e. this round has ended)
        // Why wait on gen_ != g and NOT on count_? See the lecture: a fast thread
        // could re-enter the next round and reset count_ before slow threads wake.
        // TODO
        std::unique_lock<std::mutex> lk(m_);
        int g = gen_;
        count_--;
        if (count_ == 0)
        {
            count_ = total_;
            ++gen_;
            cv_.notify_all();
        }
        else
        {
            cv_.wait(lk, [&]
                     { return gen_ != g; });
        }
    }

private:
    int total_;
    int count_;
    int gen_ = 0; // generation counter — the key to reusability
    std::mutex m_;
    std::condition_variable cv_;
};

int main()
{
    // ----- Exercise 1: expect "ping pong ping pong ... " (10 words) -----
    {
        PingPong pp;
        const int ROUNDS = 5;
        std::thread ping([&]
                         { pp.run(0, ROUNDS, "ping"); });
        std::thread pong([&]
                         { pp.run(1, ROUNDS, "pong"); });
        ping.join();
        pong.join();
        std::cout << "\n(expected: ping pong repeated " << ROUNDS << " times)\n\n";
    }

    // ----- Exercise 2: 4 threads, 1000 rounds. Barrier must not corrupt. -----
    // Each round, every thread increments a shared 'arrivals' counter (protected
    // by its own mutex), then hits the barrier. If the barrier is correct, after
    // R rounds arrivals == N*R exactly, and no thread ever races ahead.
    {
        const int N = 4, ROUNDS = 1000;
        Barrier barrier(N);
        std::mutex am;
        long long arrivals = 0;

        std::vector<std::thread> ts;
        for (int t = 0; t < N; ++t)
        {
            ts.emplace_back([&]
                            {
                for (int r = 0; r < ROUNDS; ++r) {
                    { std::lock_guard<std::mutex> lk(am); ++arrivals; }
                    barrier.arrive_and_wait();
                } });
        }
        for (auto &th : ts)
            th.join();

        long long expected = (long long)N * ROUNDS;
        std::cout << "arrivals = " << arrivals << " (expected " << expected << ")\n";
        std::cout << (arrivals == expected ? "OK\n" : "MISMATCH\n");
    }
    return 0;
}

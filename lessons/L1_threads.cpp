// L1 — Threads 101
// Compile: clang++ -std=c++20 -O2 -Wall -Wextra -pthread L1_threads.cpp -o L1 && ./L1
//
// Goals: (1) launch + join a thread, (2) pass args, (3) SEE a data race.

#include <iostream>
#include <thread>
#include <vector>
#include <string>

// ---------- TASK 1: hello from a thread ----------
// Write a function `greet(std::string name, int id)` that prints:
//   "thread <id>: hello <name>\n"
// (use std::cout <<, one statement is fine)

void greet(std::string name, int id)
{
    std::cout << "thread " << id << ": hello " << name << std::endl;
}

// ---------- TASK 2: shared counter, NO synchronization (the race) ----------
// Write a function `bump(int& counter, int times)` that does `counter++`
// in a loop `times` times. Take counter BY REFERENCE (int&).

void bump(long long &counter, long long times)
{
    for (long long i = 0; i < times; i++)
    {
        counter++;
    }
}

int main()
{
    // ----- Part 1: launch one thread, join it -----
    // TODO: create a std::thread that runs greet("world", 1), then join it.
    // Expected: "thread 1: hello world"
    std::thread t(greet, "world", 1);

    t.join();

    // ----- Part 2: launch several threads in a loop -----
    // TODO: start 3 threads running greet("t", i) for i = 0,1,2.
    // Store them in a std::vector<std::thread>, then join them all.
    // (Order of the 3 lines is non-deterministic — that's expected & fine.)
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; i++)
    {
        threads.emplace_back(greet, "t", i);
    }
    for (int i = 0; i < 3; i++)
    {
        threads[i].join();
    }

    // ----- Part 3: THE RACE -----
    // Two threads each bump the SAME counter 1,000,000 times.
    // "Correct" answer would be 2,000,000. You will very likely get LESS.
    long long counter = 0;
    // TODO: start two threads, each running bump(counter, 1'000'000).
    //   IMPORTANT: counter is passed BY REFERENCE, so you must wrap it with
    //   std::ref(counter) when constructing the thread, else it copies.
    // TODO: join both.
    long long times = 1e6;
    std::thread a(bump, std::ref(counter), times);
    std::thread b(bump, std::ref(counter), times);
    a.join();
    b.join();
    std::cout << "counter = " << counter << " (expected 2000000)\n";
    // Run it 3-4 times. Note whether the number changes between runs.

    return 0;
}

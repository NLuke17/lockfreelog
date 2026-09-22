// demo.cpp — minimal usage of the lock-free logger.
// Compile: clang++ -std=c++20 -O2 -Wall -Wextra -pthread examples/demo.cpp -o demo && ./demo

#include "../include/lockfree_logger.hpp"

int main() {
    lockfree::Logger log(stdout);          // sink = stdout; background thread drains it

    LOG(log, "hello {}", "world");
    LOG(log, "x={} y={}", 42, 7);
    LOG(log, "no placeholders here");
    LOG(log, "user {} logged in from {}", "alice", "10.0.0.1");

    for (int i = 0; i < 5; ++i)
        LOG(log, "iteration {} squared = {}", i, i * i);

    // Compile-time safety — uncomment either line and it will NOT compile:
    // LOG(log, "x={} y={}", 42);          // 2 placeholders, 1 argument
    // LOG(log, "only {}", 1, 2, 3);       // 1 placeholder, 3 arguments

    return 0;   // Logger destructor flushes the ring and joins the background thread
}

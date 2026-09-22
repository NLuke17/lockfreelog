# Lessons — Part A (Learn)

Drill files for the lock-free logging engine capstone. One lesson at a time:
I lecture the concept in chat → you fill in the numbered TODOs here → compile,
run, paste output → I grade + we discuss → next lesson.

Compile line:
```
clang++ -std=c++20 -O2 -Wall -Wextra -pthread <file>.cpp -o prog && ./prog
```

The real engine (Tiers 1–6) gets built in `../project/`, not here.

## Progress
- [x] **L1 — Threads 101** (`L1_threads.cpp`) — done: join/detach, arg-copy + `std::ref`, saw the data race (UB the optimizer hid).
- [x] **L2 — Mutex + condition_variable** (`L2_mutex_queue.cpp`) — done: bounded producer/consumer, predicate loops, clean shutdown ordering.
- [x] **L2b — More practice** (`L2b_practice.cpp`) — done: turn-taking ping-pong + reusable barrier with a generation counter.
- [x] **L3 — Atomics & the memory model** (`L3_atomics.cpp`) — done: atomic fetch_add, acquire/release handoff, and SAW relaxed reorder (193k/200k) vs seq_cst (0/200k) on ARM.
- [ ] **L4 — Compare-and-swap** (CAS loop, ABA awareness) ← current
- [ ] L5 — Cache lines & false sharing (`alignas(64)`)
- [ ] L6 — constexpr metaprogramming (compile-time format parsing)
- [ ] L7 — SIMD primer (optional bonus)

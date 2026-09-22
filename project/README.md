# Lock-Free Logging Engine — Build (Part B)

Each tier is a standalone, shippable program. Compile line:
```
clang++ -std=c++20 -O2 -Wall -Wextra -pthread <file>.cpp -o <out> && ./<out>
```

## Tiers
- [x] **T1 — Zero-alloc single-threaded ring** (`t1_ring.cpp`) — done. Power-of-two mask,
      monotonic head/tail counters, full→drop, drain. No heap alloc in `push`.
- [x] **T2 — Background flush thread (mutex)** (`t2_flush.cpp`) — done. Correct producer/consumer,
      clean shutdown (drain-then-exit), ~7–39% drops from holding the lock across I/O (the T3 motivation).
- [ ] **T3 — Lock-free SPSC** (`t3_spsc.cpp`) ⭐ the headline: atomic head/tail, acquire/release. ← current
- [x] **T5 — Cache alignment + benchmark** (`t5_bench.cpp`, `t5_libs.cpp`, `t5_micro.cpp`) — done.
      alignas(128) on M3; false sharing 4.21x isolated; ~30M enqueue / ~42M sustained; 3.8x vs spdlog-async. See RESULTS.md.
- [x] **T4 — Compile-time format parsing** (`t4_format.cpp`) — done. consteval placeholder count,
      C++20 string-literal NTTP, if constexpr type dispatch, static_assert rejects wrong arity at compile time.
- [ ] **T6 — SIMD string copy** (`t6_simd.cpp`) — AVX2/NEON payload copy (bonus).

## Design invariants (hold across all tiers)
- `head_` = total written, `tail_` = total read; both monotonic uint64, never reset.
- slot index = `counter & (CAPACITY-1)`; CAPACITY is a power of two.
- SPSC: producer is sole writer of `head_`, consumer is sole writer of `tail_`.
- Full buffer → producer drops the incoming message (never blocks, never writes `tail_`).

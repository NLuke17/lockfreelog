# Benchmark results (Apple M3, clang++ -std=c++20 -O2, 128B cache line)

These are the REAL measured numbers. The resume bullet must match these — no inflation.

## Throughput
| metric | number | source |
|---|---|---|
| enqueue throughput (app-visible LOG cost) | **~28 M msgs/sec** | `t5_micro.cpp` (B) |
| sustained end-to-end (light drain) | **~41 M msgs/sec** | `t5_bench.cpp` aligned |
| sustained end-to-end (real per-msg stdio drain) | ~28 M msgs/sec | `t5_libs.cpp` |
| vs spdlog async | **~4x faster** (3.8–4.7x across runs) | `t5_libs.cpp` |

## Cache-line alignment (false sharing)
| layout | ops/sec | source |
|---|---|---|
| indices adjacent (same line) | 258 M/s | `t5_micro.cpp` (A) |
| indices aligned (separate 128B lines) | 1086 M/s | `t5_micro.cpp` (A) |
| **speedup from alignment (isolated)** | **4.21x** | |
| speedup in full logger | ~1–9% (payload copy dominates) | `t5_bench.cpp` |

## vs libraries (5M msgs, sink=/dev/null), M msgs/sec
| logger | rate | note |
|---|---|---|
| ours (async lock-free) | 22.5 | real fwrite drain |
| spdlog (async) | 5.9 | **we are 3.8x faster** |
| fmt (sync) | 50.1 | buffered memcpy to /dev/null (no real I/O, no threading) |
| printf (sync) | 19.4 | we are 1.2x faster |
| iostream (sync) | 39.7 | buffered memcpy to /dev/null |

## Correctness
- T3: 5,000,000 msgs, checksum match (byte-exact across lock-free boundary), 0 drops.
- Lock-free SPSC verified; acquire/release handoff proven by checksum equality.

## Open: chase 50M via cached-index optimization (rigtorp/Folly technique) — TODO after T4/T6.

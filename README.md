# Lock-Free Logging Engine

A low-latency, **lock-free** logging engine in **C++20**, built to production-grade design
standards: a wait-free producer, zero heap allocation on the hot path, and a background
thread that absorbs all I/O. Benchmarked against `spdlog`, `fmt`, `iostream`, and `printf`.

> Design rule of the house: **every claim here is measured and defensible.** Numbers are
> from an Apple M3 (`clang++ -std=c++20 -O2`); reproduce them with the benchmark files below.

---

## Highlights

- **Lock-free single-producer / single-consumer (SPSC) ring buffer.** No mutex, no CAS on
  the hot path. Producer and consumer coordinate through two `std::atomic<uint64_t>` indices
  with `acquire`/`release` ordering.
- **Wait-free producer.** `push()` is a fixed, bounded instruction sequence — no loops, no
  retries, never blocks on the consumer. On a full buffer it drops rather than stall the app.
- **Zero heap allocation on the hot path.** The ring is a fixed, preallocated `std::array`
  inside the object; `push()` never calls `new`/`malloc`.
- **~28M msgs/sec enqueue, ~41M sustained — ≈4× faster than spdlog's async logger.**
- **Cache-line-aligned indices** (`alignas(128)` on Apple Silicon) eliminate false sharing:
  **4.2× on the contended indices in isolation.**
- **Compile-time format-string parsing** (C++20 NTTP + `consteval`): `LOG<"x={}">(a, b)`
  with the wrong number of arguments is a **compile error**, not a runtime bug.
- **Correctness verified**: 5,000,000 messages cross the lock-free boundary with a
  **byte-exact checksum** and zero drops.

---

## Architecture

```
  app thread (producer)                      background thread (consumer)
  ─────────────────────                      ────────────────────────────
  LOG("x={}", v)                             loop:
    format into slot                           acquire-load head_  ─────┐
    ... ring buffer (fixed, preallocated) ...                          │ sees published
    release-store head_  ───────────────────►  read slots [tail_,head_)│ slot bytes
                          ◄───────────────────  release-store tail_     │
    acquire-load tail_  (is there room?)         write to sink (I/O)  ◄─┘
    full? -> DROP (never block)

  head_ = total written (only producer writes)   ─┐ one writer per index →
  tail_ = total read    (only consumer writes)   ─┘ no lock, no CAS needed
  slot index = counter & (CAPACITY-1)              (power-of-two mask, 1 instruction)
```

The producer publishes a message by `release`-storing `head_`; the consumer `acquire`-loads
it and is then guaranteed to see the slot bytes written before the store (a *synchronizes-with
→ happens-before* edge). That single handoff is the whole lock-free design.

---

## Design decisions (the "why")

| Decision | Why |
|---|---|
| **SPSC, no CAS** | Each index has exactly one writer (producer→`head_`, consumer→`tail_`), so there's no write-write race to arbitrate — the only thing a lock or CAS is for. |
| **`acquire`/`release`, not `seq_cst`** | The handoff is a one-directional publish/subscribe on one variable between two threads. `seq_cst` would add a global total order (a full `dmb ish` barrier on ARM) we don't need — pure cost. Use the weakest ordering that's still correct. |
| **Monotonic 64-bit indices** | `head_ - tail_` gives the count and distinguishes full from empty; the low bits are the slot. Never reset; modular unsigned arithmetic stays correct even through overflow (~5,800 years at 100M/s anyway). |
| **Full → drop** | A logger must never stall the app. Dropping is O(1) and preserves SPSC (overwriting oldest would require the producer to write `tail_`, breaking the single-writer invariant). |
| **`alignas(128)` on indices** | `head_` (producer-written) and `tail_` (consumer-written) would otherwise share a 128-byte cache line (verified via `sysctl hw.cachelinesize`), ping-ponging it between cores on every op — false sharing. |
| **Compile-time format parsing** | Wrong-arity logs fail to compile; no runtime format-string reparsing; `std::to_chars` keeps conversion allocation- and locale-free. Also kills format-string vulnerabilities (the format must be a compile-time literal). |

---

## Benchmarks (Apple M3, `-O2`)

**Throughput** (5M messages, sink = `/dev/null`):

| Logger | msgs/sec | notes |
|---|---:|---|
| **this engine (async lock-free)** | **~28M** | app-visible enqueue cost |
| spdlog (async) | ~6M | **≈4× slower** — the fair async-vs-async comparison |
| printf (sync) | ~22M | we are ~1.3× faster |
| iostream (sync) | ~41M | buffered memcpy to /dev/null (no real I/O, no threading) |
| fmt (sync) | ~52M | same caveat — measures buffered memcpy, not a realistic sink |

**Cache-line alignment / false sharing** (isolated, two cores incrementing independent atomics):

| Layout | ops/sec | |
|---|---:|---|
| indices adjacent (same line) | ~259M | false sharing |
| indices aligned (separate lines) | ~1091M | **4.21× faster** |

In the *full* logger the alignment win is ~7% — the 64-byte payload copy dominates per-message
cost, and `tail_` is published once per drained batch. (Microbenchmarks exaggerate contention;
both numbers are reported honestly.)

**Correctness:** 5,000,000 messages, consumer folds every byte into a rolling hash, producer
computes the same hash independently → **byte-exact match, zero drops**. A single wrong memory
order would corrupt the payload and diverge the hash.

---

## Build & run

```bash
# core tiers (no external deps)
clang++ -std=c++20 -O2 -Wall -Wextra -pthread project/t3_spsc.cpp  -o t3      && ./t3
clang++ -std=c++20 -O2 -Wall -Wextra -pthread project/t5_micro.cpp -o t5micro && ./t5micro
clang++ -std=c++20 -O2 -Wall -Wextra          project/t4_format.cpp -o t4     && ./t4

# library comparison (needs: brew install fmt spdlog)
clang++ -std=c++20 -O2 -Wall -Wextra -pthread \
  -I/opt/homebrew/include -L/opt/homebrew/lib \
  project/t5_libs.cpp -o t5libs -lspdlog -lfmt && ./t5libs
```

---

## Honest findings & limitations

- **SIMD (NEON) string copy** is implemented and correct, but **did not beat the compiler's
  auto-vectorized copy / libc `memcpy`** for 64-byte payloads — modern toolchains already
  vectorize small copies. Hand-rolled SIMD pays off for large or non-trivial transforms, not
  short log lines. Kept as an honest, measured result rather than an inflated claim.
- **SPSC only.** A real logger with many app threads is **MPSC**: producers would contend on
  `head_`, resolved with a `compare_exchange_weak` reserve loop (lock-free, no longer
  wait-free). ABA is a non-issue — the indices are monotonic. This is the natural next step.
- **Consumer spins/yields when empty** — a latency-vs-CPU tradeoff; a condvar would re-introduce
  a wake syscall on the producer path.

---

## Repository layout

```
project/    the engine, built in tiers (each a standalone program)
  t1_ring.cpp     zero-alloc single-threaded ring buffer
  t2_flush.cpp    background flush thread (mutex version — correct baseline)
  t3_spsc.cpp     lock-free SPSC ⭐  (the core)
  t4_format.cpp   compile-time format-string parsing
  t5_bench.cpp    cache-line alignment + benchmark harness
  t5_libs.cpp     comparison vs spdlog / fmt / iostream / printf
  t5_micro.cpp    isolated false-sharing + enqueue microbenchmarks
  t6_simd.cpp     NEON SIMD payload copy (explored)
  RESULTS.md      raw measured numbers
lessons/    the learning path (threads, mutexes, atomics, memory model, constexpr)
```

Built in stages — *correct before fast*: the mutex version (T2) came first as a baseline,
then the lock-free version (T3) removed the lock and the speedup was **measured**, not assumed.

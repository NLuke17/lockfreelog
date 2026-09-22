# Lock-Free Logging Engine

A header-only, **lock-free**, zero-allocation logging engine in **C++20**. The application
thread formats a message directly into a preallocated ring slot and publishes it with a
single atomic store — no lock, no heap allocation, never blocking. A background thread drains
the ring to the sink, keeping all I/O off the hot path.

```cpp
#include "lockfree_logger.hpp"

lockfree::Logger log(stderr);                          // background flush thread starts
LOG(log, "user {} logged in from {}", user_id, ip);    // format checked at COMPILE TIME
```

> Every number below is measured on an Apple M3 (`clang++ -std=c++20 -O2`) and reproducible
> with the programs in `bench/`. Design rule: no claim that isn't measured.

---

## Highlights

- **Lock-free single-producer / single-consumer ring buffer** — producer and consumer
  coordinate through two `std::atomic<uint64_t>` indices with `acquire`/`release` ordering.
  No mutex, no CAS on the hot path.
- **Wait-free producer** — `LOG(...)` is a fixed, bounded instruction sequence: it never
  loops, retries, or blocks on the consumer. On a full buffer it drops rather than stall.
- **Zero heap allocation** — the ring is a fixed `std::array` inside the object; the hot path
  never calls `new`/`malloc`. Messages format in place with `std::to_chars` (no allocation).
- **~28M msgs/sec sustained — 5.3× faster than spdlog's async logger, 2.3× faster than printf.**
- **128-byte cache-line-aligned indices** eliminate false sharing: **4.1× on the contended
  indices in isolation.**
- **Compile-time format-string parsing** (C++20 NTTP + `consteval`) — a wrong-argument-count
  `LOG` is a **compile error**, and the format can't be a runtime format-string exploit.

---

## Usage

```cpp
#include "lockfree_logger.hpp"

int main() {
    lockfree::Logger log(stdout);          // or any FILE*: stderr, an fopen'd file

    LOG(log, "hello {}", "world");
    LOG(log, "x={} y={}", 42, 7);
    LOG(log, "no placeholders here");

    // LOG(log, "x={} y={}", 42);          // <-- compile error: 2 placeholders, 1 arg

    return 0;   // destructor flushes the ring and joins the background thread (RAII)
}
```

`Logger<Capacity, MaxMsgLen>` is templated on compile-time sizes (defaults `8192`, `256`),
so the entire buffer is preallocated — there is no runtime allocation anywhere.

---

## How it works

```
  app thread (producer)                      background thread (consumer)
  ─────────────────────                      ────────────────────────────
  LOG("x={}", v)                             loop:
    acquire-load tail_ (room?) ── full? DROP    acquire-load head_  ─────┐ sees the
    format directly into slot                   read slots [tail_,head_) │ published
    release-store head_  ───────────────────►   coalesce -> one fwrite   │ slot bytes
                          ◄───────────────────   release-store tail_    ◄─┘

  head_ = total written (only the producer writes it)  ─┐ one writer per index →
  tail_ = total read    (only the consumer writes it)  ─┘ no lock, no CAS needed
  slot index = counter & (Capacity-1)                    (power-of-two mask, 1 instruction)
```

The producer publishes by `release`-storing `head_`; the consumer `acquire`-loads it and is
then guaranteed to see the slot bytes written before the store (*synchronizes-with →
happens-before*). That one handoff is the entire lock-free design.

### Design decisions (the "why")

| Decision | Why |
|---|---|
| **SPSC, no CAS** | Each index has exactly one writer, so there's no write-write race to arbitrate — the only thing a lock or CAS is for. |
| **`acquire`/`release`, not `seq_cst`** | A one-directional publish/subscribe on one variable between two threads. `seq_cst` would add a global total order (a full `dmb ish` barrier on ARM) that isn't needed — pure cost. Use the weakest ordering that's still correct. |
| **Monotonic 64-bit indices** | `head_ - tail_` gives the count and distinguishes full from empty; the low bits are the slot. Modular unsigned arithmetic stays correct even across overflow (~5,800 years at 100M/s). |
| **Full → drop** | A logger must never stall the app. Dropping is O(1) and preserves SPSC (overwriting oldest would need the producer to write `tail_`, breaking the single-writer invariant). |
| **`alignas(128)` indices** | `head_` (producer) and `tail_` (consumer) would otherwise share a 128-byte cache line (verified via `sysctl hw.cachelinesize`), ping-ponging it between cores every op — false sharing. |
| **Compile-time format parsing** | Wrong-arity logs fail to compile; no runtime format reparse; `std::to_chars` keeps conversion allocation- and locale-free; the format must be a literal, so no format-string exploits. |
| **Coalesced drain** | The consumer batches a whole drain pass into one `fwrite`, so it keeps up with the producer instead of paying a syscall per message. |

---

## Benchmarks

**Throughput scales with the hardware** — measured on two platforms:

| Platform | this engine | notes |
|---|---:|---|
| **Intel Xeon Gold 6348** (Ice Lake, x86, Linux) | **~50M msgs/sec** | server-class core, 64-byte cache line |
| **Apple M3** (ARM, macOS) | **~28M msgs/sec** | laptop, 128-byte cache line, weakly ordered |

The gap is expected: the x86 server has higher single-core throughput and a stronger (TSO)
memory model, so the acquire/release handoff is cheaper. `alignas(128)` is a portable choice —
it fully separates the indices on both (on x86 it simply over-aligns past the 64-byte line).

**Detail (Apple M3, `-O2`)** — every logger formats and emits the same line to `/dev/null`
(5M messages):

| Logger | msgs/sec | |
|---|---:|---|
| **this engine (async lock-free)** | **~28M** | 0 messages lost |
| fmt (sync) | ~27M | buffered memcpy to /dev/null (no real I/O, no threading) |
| printf (sync) | ~12M | **ours 2.3× faster** |
| iostream (sync) | ~7M | |
| spdlog (async) | ~5M | **ours 5.3× faster** (the fair async-vs-async comparison) |

**Cache-line alignment / false sharing** (`bench/false_sharing.cpp`, isolated):

| Layout | ops/sec | |
|---|---:|---|
| indices adjacent (same line) | ~270M | false sharing |
| indices aligned (separate lines) | ~1097M | **4.1× faster** |

In the full logger the alignment win is ~7% — the payload copy dominates per-message cost.
(Microbenchmarks exaggerate contention; both numbers are reported honestly.)

**Correctness** — a separate SPSC test pushes 5,000,000 messages while the consumer folds
every byte into a rolling hash; the producer computes the same hash independently. They match
byte-for-byte, which proves the acquire/release handoff delivers every payload intact — a
single wrong memory order would diverge the hash.

---

## Build & run

```bash
# example (no external deps)
clang++ -std=c++20 -O2 -Wall -Wextra -pthread examples/demo.cpp -o demo && ./demo

# cache-line / false-sharing microbenchmark
clang++ -std=c++20 -O2 -Wall -Wextra -pthread bench/false_sharing.cpp -o fs && ./fs

# throughput vs spdlog / fmt / iostream / printf   (brew install fmt spdlog)
clang++ -std=c++20 -O2 -Wall -Wextra -pthread \
  -I/opt/homebrew/include -L/opt/homebrew/lib \
  bench/benchmark.cpp -o benchmark -lspdlog -lfmt && ./benchmark
```

Header-only: just `#include "include/lockfree_logger.hpp"`.

---

## Limitations & next steps

- **SPSC only.** A logger with many app threads is **MPSC**: producers would contend on
  `head_`, resolved with a `compare_exchange_weak` reserve loop (lock-free, no longer
  wait-free). ABA is a non-issue — the indices are monotonic. This is the natural next step.
- **Consumer spins/yields when empty** — a latency-vs-CPU tradeoff; a condition variable would
  re-introduce a wake syscall on the producer path.
- **Truncation** — messages longer than `MaxMsgLen` are truncated to keep the hot path
  branch-light and allocation-free.

---

## Layout

```
include/lockfree_logger.hpp   the engine (header-only)
examples/demo.cpp             minimal usage
bench/benchmark.cpp           throughput vs spdlog / fmt / iostream / printf
bench/false_sharing.cpp       isolated cache-line-alignment microbenchmark
```

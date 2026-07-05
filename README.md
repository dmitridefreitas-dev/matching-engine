# matching-engine

[![CI](https://github.com/dmitridefreitas-dev/matching-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/dmitridefreitas-dev/matching-engine/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

📄 **[Report (PDF)](analysis/matching-engine-report.pdf)** · 📓 [analysis notebook](analysis/benchmarks.ipynb)

**A C++20 price-time-priority limit-order-book matching engine, built twice on
purpose: a readable `std::map` reference and a cache-aware optimized engine —
differentially fuzzed against each other until indistinguishable, then benchmarked
on a replayed LOBSTER market-data day with the methodology stated before the
numbers.** Limit and market orders, cancels, priority-preserving reduces, partial
fills. Header-only, no dependencies beyond GoogleTest for the tests; ASan/UBSan run
on every CI build.

## The two implementations

| | `MapBook` (reference) | `FastBook` (optimized) |
|---|---|---|
| price levels | `std::map` (tree walk per lookup) | **contiguous price ladder** — one array slot per tick, level lookup is one add + one load |
| FIFO queue per level | `std::list` (heap node per order) | **intrusive doubly-linked list** inside an **object pool** with a free list — zero steady-state allocation, O(1) mid-queue cancel |
| best bid/ask | `map.begin()` | maintained ladder indices, advanced by amortised scan on level exhaustion |
| role | visibly correct; the **oracle** | the engine being proven and measured |

Same semantics contract, enforced twice over: one GoogleTest suite runs against both
implementations via typed tests, and a **differential fuzzer** feeds identical random
order streams to both engines, asserting after *every operation* that fills, return
values, and (at checkpoints) full book snapshots match exactly, plus invariants — no
crossed book, levels sorted, FIFO preserved, no zero-quantity residents, quantity
conservation on every trade. 25 seeds × 20k ops plus a 200k-op session per test run,
under sanitizers in CI. A failure prints its seed: every counterexample is one
command from a deterministic repro.

## Measured results

Setup: AMD Ryzen 7 7730U (Windows 11 laptop), clang++ (LLVM-MinGW) `-O2`, single
thread pinned to one core, full warmup replay excluded, median of 3 runs (min–max
whiskers), identical op streams per engine. Latency is TSC-timed per operation —
Windows `steady_clock` has 100ns granularity, which would quantise the percentiles —
and the ~5–10ns rdtsc-pair cost is *included*. Full details in the
[report](analysis/matching-engine-report.pdf).

Flows: **synthetic** (1M ops, 55/25/10/10 submit/cancel/market/reduce around a
random-walking mid, heavy-tailed sizes) and a **LOBSTER replay** — the full AMZN
2012-06-21 sample day (269,748 messages → 261k engine ops after mapping; executions
synthesised as opposite-side marketable orders; `scripts/download_lobster.py`
fetches the file).

| flow | engine | throughput (median) | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|---|---|
| synthetic | MapBook | 8.2 M ops/s | 90 ns | 330 ns | 951 ns | 3.3 µs |
| synthetic | **FastBook** | **12.2 M ops/s (1.49×)** | **50 ns** | **180 ns** | **641 ns** | **2.1 µs** |
| LOBSTER | MapBook | 7.5 M ops/s | 100 ns | 180 ns | **310 ns** | **671 ns** |
| LOBSTER | **FastBook** | **10.7 M ops/s (1.44×)** | **60 ns** | **160 ns** | 380 ns | 751 ns |

![Throughput](assets/throughput.png)

![Latency percentiles](assets/latency.png)

**The honest finding is in the LOBSTER tail.** FastBook halves the median but its
p99/p99.9 are slightly *worse* than MapBook's on the replay — measured, not hidden.
That is the price ladder's known weakness on sparse books: AMZN ticks in $0.0001 on
a ~$224 stock, so occupied levels sit hundreds of empty ticks apart, and when a best
level empties the ladder scans linearly for the next occupied slot while the tree
steps to its neighbour. Dense books never show this; wide-tick real books do. The
standard fix — a bitmap summary of occupied levels scanned with `tzcnt` — is the
named next step. Rare 0.5–10ms max-latency spikes on both engines are page faults
and OS preemption on a desktop laptop; this repo's claim is the *relative*
comparison under identical conditions, not absolute HFT figures.

## Build and run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build                      # 33 tests
./build/lob_bench --ops 1000000 --seed 42   # synthetic benchmark

python scripts/download_lobster.py          # fetch the AMZN sample (~11 MB)
./build/lob_bench --flow lobster --file data/AMZN_message.csv
```

Sanitized build: `cmake -B build -DLOB_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug` — the
same configuration CI runs on every push (gcc + clang × Release + ASan/UBSan).

## Semantics contract (what the fuzzer holds both engines to)

- **Price-time priority**: better price first; FIFO within a level.
- Fills execute at the **maker's** price — taker price improvement, as on exchanges.
- Limit remainders **rest**; market remainders are **discarded**.
- `reduce` lowers quantity **in place, preserving time priority** (exchange
  semantics); size-ups and price changes are cancel+replace, composed by the caller.
- Integer ticks and integer quantities throughout — no floating point in the book.

## Limitations / next steps

- **Single-threaded matching core only** — no sessions, gateway, or persistence;
  the interesting concurrency question (SPSC queue in front of a single matching
  thread, the standard exchange shape) is deliberately out of scope for v1.
- **Ladder rescan on sparse books** (measured above) — bitmap level summary next.
- **`std::unordered_map` for id→order lookup** — the honest chokepoint left in
  both engines; open addressing keyed by dense ids is the production answer.
- **No self-trade prevention, IOC/FOK, or iceberg orders** — semantics extensions
  that slot into the same differential harness.
- LOBSTER replay synthesises aggressors from execution prints (stated in
  `lobster.hpp`) — realistic *load*, not a tick-exact reconstruction of the tape.

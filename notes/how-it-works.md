# How this project works — study notes

Plain-English walkthrough. Read alongside the README and `analysis/benchmarks.ipynb`.

## What this project is

A limit-order-book matching engine — the data structure at the heart of every
exchange — built **twice**: once for clarity (`MapBook`) and once for speed
(`FastBook`), with the clear version serving as the correctness oracle for the fast
one. The deliverable is not either engine alone; it is the *method*: differential
fuzzing until the two are indistinguishable, then benchmarks whose methodology is
stated before their numbers, on real replayed market data, with the place where the
optimized design loses reported as prominently as the places it wins.

## What a matching engine does

Orders arrive as a stream: "buy 100 @ $10.05", "cancel order 338", "sell 50 at
market". The book keeps every resting order organised by **price-time priority**:
best price trades first, and at the same price, first-come-first-served (FIFO). An
incoming order that crosses the spread *matches* against resting orders — fills
happen at the resting (maker) price — and any unmatched remainder either rests (limit)
or is discarded (market). Cancels remove resting orders; reduces shrink them in place
without losing their queue position (real exchange semantics: size down keeps
priority, size up goes to the back via cancel+replace).

## Why build it twice

Optimized code is where bugs hide — intrusive pointers, manual memory pools, and
index arithmetic are exactly the things code review misses. The defence is an oracle:
`MapBook` uses the most obvious container for every job (`std::map` of price levels,
`std::list` per level), so its correctness is close to inspectable. Then the
**differential fuzzer** throws hundreds of thousands of randomized operations at both
engines and demands bit-identical behaviour: same fills in the same order, same
return values, same book snapshots, plus standing invariants (bids below asks, levels
sorted, no zero-quantity residents, every trade conserving quantity). Any divergence
prints its seed and becomes a deterministic repro. This is the same honesty
architecture as the rest of the portfolio — options-pricing-lib cross-validates three
pricers; here one implementation cross-validates another.

## What makes FastBook fast

Five ideas — three from v1, two added in v2 after the benchmarks located the
remaining costs:

1. **Price ladder instead of a tree.** Prices are integer ticks in a bounded range,
   so levels live in a plain array indexed by `price - min_price`. Finding a level is
   one subtraction and one load — no tree walk, no pointer chasing, and neighbouring
   prices are neighbouring memory (cache-friendly for the clustered-around-mid access
   pattern of real flow).
2. **Object pool instead of per-order allocation.** All orders live in one
   `std::vector`; freed slots go on a free list and get recycled. Steady state does
   zero allocation, and pool indices are 4-byte handles instead of 8-byte pointers.
3. **Intrusive FIFO queues.** Each order node carries prev/next pool indices; each
   ladder slot carries head/tail. Appending is O(1), and cancelling from the middle
   of a queue is O(1) — no searching, no allocator involvement.
4. **Occupancy bitmap (v2).** One bit per price level; when a best level empties,
   the next occupied level is found by scanning 64 ticks per `countr_zero`/
   `countl_zero` instruction instead of one tick per loop iteration. This is what
   fixed the sparse-book tail (below).
5. **Open-addressed id map (v2, `id_map.hpp`).** The id→slot lookup — touched by
   every submit, fill, and cancel — was a `std::unordered_map`: per-node allocation
   and bucket pointer-chasing, exactly the costs everything else avoids. Replaced
   with a flat linear-probing table using Fibonacci hashing and **backward-shift
   deletion** (no tombstones, so lookups never degrade as the book churns).

## What the benchmarks showed — and the v1 → v2 receipts

v1 measured two weaknesses and reported them instead of hiding them; v2 fixed
exactly those two things and re-measured (v1 baseline preserved in
`results/benchmarks_v1_baseline.csv`):

- **Throughput:** now ~2.1× (14.1 vs 6.8 M ops/s on the AMZN replay; 11.9 vs 5.5
  synthetic), up from ~1.5× in v1.
- **Median latency:** p50 ~30ns vs MapBook's ~100–110ns (3.3–3.7×), halved again
  from v1's ~50–60ns.
- **The tail, fixed with receipts:** v1's honest crossover — LOBSTER p99/p99.9
  *worse* than MapBook because the best-level rescan walked hundreds of empty
  $0.0001 ticks — inverted after the bitmap: p99 380 → 330ns (MapBook ~360),
  p99.9 751 → 510ns (MapBook ~881). Because only those two changes landed between
  the runs, the deltas are attributable.
- **What stayed honest:** synthetic whole-replay throughput medians barely moved
  despite every latency percentile improving — wall-clock medians over 3 runs on a
  desktop laptop carry OS noise that million-sample latency percentiles do not.
  Rare 0.5–10ms max spikes (page faults, preemption) remain on both engines. The
  claim is the relative comparison under identical conditions, not absolute HFT
  numbers — stated in the README exactly that way.

## How the LOBSTER replay works

LOBSTER message files record a real Nasdaq day per instrument: submissions, partial
cancels, deletions, executions, in $0.0001 ticks. Mapping: type 1 → submit, type 2 →
reduce (LOBSTER cancels *by amount*; the parser tracks live sizes to convert to
reduce-to), type 3 → cancel, type 4 (an execution print) → a synthesised marketable
order on the *opposite* side at the printed price/size — reproducing realistic load,
not a tick-exact tape reconstruction (stated in the header comment). The parser is
tested against a hand-written 8-row fixture exercising every rule, plus a
differential-consistency replay.

## Testing discipline signals worth knowing

- One semantics suite, two engines, via GoogleTest **typed tests** — the contract is
  written once and both implementations must pass it by name.
- CI is a **gcc + clang × Release + ASan/UBSan matrix**; the sanitizer runs execute
  the entire differential fuzz, so every memory error class the pool/intrusive-list
  code could introduce is being hunted on every push.
- The benchmark binary itself asserts both engines produced identical fill counts on
  the measured stream — even the perf harness refuses to bless divergent engines.

## Likely interview questions

- *Why is reduce-in-place priority-preserving but size-up not?* Exchange fairness:
  shrinking your order takes liquidity away from no one behind you; growing it would
  let you add size while keeping time priority you didn't earn for the new shares.
- *Why integer ticks?* Prices are discrete on real venues; floats introduce equality
  and ordering hazards inside the hottest comparison in the system, and the ladder
  indexing *requires* integers.
- *What breaks if two orders share an id?* Documented precondition (unique ids per
  session), asserted in debug; the id→locator maps would silently misroute cancels
  otherwise. A production gateway enforces this upstream.
- *Why not benchmark with hyper-optimised flags (-O3, PGO, march=native)?* -O2 is
  the honest default; the comparison is between designs, and both engines get the
  same flags. Flag-tuning both would shift absolute numbers, not the story.
- *Why does backward-shift deletion matter for an order book specifically?* Books
  are pure churn — most orders are cancelled, not filled, so the id table sees
  near-equal insert and erase rates forever. Tombstone deletion makes probe chains
  grow monotonically under that workload; backward-shift keeps the table as clean
  as if the erased keys had never existed.
- *What would you do next for latency?* An SPSC-queue front end with a batch-drain
  matching loop (the standard single-matching-thread exchange shape), huge pages
  and locked memory for the pool and ladder, and an isolated-core Linux rig so the
  0.5–10ms OS-noise spikes stop polluting the max.

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

Three ideas, each attacking a specific cost in MapBook:

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

Best bid/ask are maintained as ladder indices; when the best level empties, the index
scans toward the next occupied level. That scan is the design's honest weakness —
see below.

## What the benchmarks showed

- **Throughput:** ~1.5× (12.2 vs 8.2 M ops/s synthetic; 10.7 vs 7.5 on the AMZN
  replay), median of 3 pinned, warmed-up runs.
- **Median latency:** roughly halved (p50 ~50ns vs ~90ns).
- **The honest crossover:** on the LOBSTER replay, FastBook's p99/p99.9 are slightly
  *worse* than MapBook's. Cause, not hand-waving: AMZN's tick is $0.0001 on a ~$224
  stock, so occupied levels sit hundreds of empty ticks apart, and the
  best-level-emptied rescan walks them one by one, while `std::map` steps to its
  neighbour in O(log n). Dense synthetic books never show this; sparse real books
  do. Fix (named, not done): a bitmap of occupied levels — one bit per tick, scan
  64 ticks per `tzcnt` instruction.
- **Tail spikes** (0.5–10ms max on both engines): page faults and preemption on a
  desktop laptop. The claim is the relative comparison under identical conditions,
  not absolute HFT numbers — stated in the README exactly that way.

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
- *What would you do next for latency?* The bitmap level summary (kills the sparse
  rescan), open-addressed id table (kills the unordered_map), then an SPSC-queue
  front end and batch-drain loop — the standard single-matching-thread exchange
  shape.

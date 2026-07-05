# Code walkthrough — how every file actually works

Companion to `how-it-works.md` (concepts). This is the code-level defense.

## Map

| File | Role |
|---|---|
| `include/lob/types.hpp` | `Fill`, `LevelSnapshot`, integer tick/qty types |
| `include/lob/map_book.hpp` | reference engine (`std::map` + `std::list`) |
| `include/lob/fast_book.hpp` | optimized engine (ladder + pool + intrusive lists) |
| `include/lob/flow.hpp` | deterministic synthetic flow generator |
| `include/lob/lobster.hpp` | LOBSTER message-file → engine-ops parser |
| `include/lob/tsc.hpp` | rdtsc timing + wall-clock calibration |
| `tests/test_semantics.cpp` | one contract, both engines (typed tests) |
| `tests/test_equivalence.cpp` | the differential fuzzer |
| `tests/test_lobster.cpp` | parser rules on a hand-written fixture |
| `bench/bench_main.cpp` | pinned, warmed, 2-mode benchmark harness |

## `types.hpp`

`Price = int64` ticks, `Quantity = uint32`, `OrderId = uint64`. `Fill{taker, maker,
price, quantity}` with `operator==` — fills are the canonical output the
differential fuzzer compares. `LevelSnapshot` is the slow-path "what does this level
look like" view both engines can emit, used only by tests.

## `map_book.hpp` — the reference, line by line

- `bids_` is `std::map<Price, Level, std::greater<Price>>` so `begin()` is the BEST
  bid; asks use the default `std::less` so `begin()` is the best ask. One comparator
  choice replaces a mess of `rbegin()` special cases.
- A `Level` is `std::list<Resting>` — chosen over `deque`/`vector` because **list
  iterators are stable**, which is what makes the O(1) cancel index possible:
  `index_ : id -> {side, price, list-iterator}`.
- `match()` is a template over the opposite-side map and a `crosses(best_price)`
  lambda: buy-limits pass `best <= limit`, sell-limits `best >= limit`, market
  orders `always true`. One matching loop serves all four order paths — fewer
  places for the semantics to diverge.
- The inner loop fills against `level.front()` (FIFO), erases fully-consumed makers
  from both the level and the id index, and erases the level itself when empty —
  which is exactly the "no empty level survives" invariant the fuzzer asserts.
- `reduce()`: found → must be strictly smaller → write through the stored iterator
  (`found->second.node->qty = new_qty`) — the order never moves, which IS the
  priority preservation.

## `fast_book.hpp` — the optimized engine, line by line

- **Construction**: `bid_levels_`/`ask_levels_` are `vector<Level>` of size
  `max_price - min_price + 1`; `Level` is just `{head, tail}` pool indices (NIL =
  `UINT32_MAX`). `best_bid_ = -1`, `best_ask_ = ladder_size` are the "empty"
  sentinels — signed `int64` indices so the empty states are representable.
- **`to_index(price)`** bounds-checks and throws `std::out_of_range` — one branch
  per op buys memory safety on bad input (silent UB in a book would be
  disqualifying).
- **`rest()`**: pop a slot from the free list (or grow the pool), write the node,
  splice it onto the level's tail (`prev = old tail`, fix `tail->next`, update
  `tail`), update `index_`, and bump the best index if this level is better —
  `idx > best_bid_` / `idx < best_ask_` are the only comparisons needed because
  resting can only ever *improve* the best.
- **`consume_level()`**: fills against `level.head` (FIFO), and on full consumption
  pops the head (`level.head = maker.next; head->prev = NIL`), releases the slot,
  erases the id. The caller (`match_asks`/`match_bids`) advances the best index with
  a `do/while` scan when the level empties. The scan is the measured sparse-book
  weakness — see the README's LOBSTER tail discussion.
- **`unlink_and_free()`** (cancels): classic doubly-linked unlink via pool indices —
  four cases collapse to two ternary-free branches for prev and next. Then the
  subtle part: **a cancel can empty the best level**, so if the emptied level was
  the best, rescan. Forgetting that rescan leaves `best_ask_` pointing at an empty
  level and the next match loop spins — the differential fuzzer caught exactly this
  class of bug during development; that is what the fuzzer is *for*.
- **`allocate()/release()`**: LIFO free list — the most recently freed slot is the
  next reused, which is also the cache-warmest.

## `flow.hpp` — deterministic randomness

All randomness is `mt19937_64() % n` — **not** `std::uniform_int_distribution`,
which is implementation-defined and would produce different streams on libstdc++ vs
libc++, breaking "same seed, same stream, any platform". The mix (55% submits, 25%
cancels, 10% markets, 10% reduces; 25% of limits aggressive; heavy-tailed sizes;
random-walking mid) is realistic in *shape* and labelled as such. Cancel/reduce
targets are drawn from all ids ever issued — stale targets (already filled) are
deliberate edge-case coverage, since both engines must agree on returning `false`.

## `lobster.hpp` — the replay mapping

Hand-rolled CSV field walker with `std::from_chars` (no iostream-per-field, no
locale). Type 2 rows cancel *by amount*, so the parser tracks live sizes in a map to
convert them into the engine's reduce-to semantics; type 4 execution prints become
synthesised marketable orders on the opposite side (ids start at 10^12 to dodge the
exchange id space); types 5–7 are counted and skipped. `LobsterStats` reports
everything skipped or synthesised — the README's "realistic load, not tick-exact
tape" caveat is backed by these counters.

## `tsc.hpp` and the two-mode benchmark

`steady_clock` on Windows is QueryPerformanceCounter at 100ns granularity — the
first benchmark run produced p50 = exactly 100ns for BOTH engines, which was the
clock quantising, not the code (kept in the git history as the honest lesson).
`__rdtsc()` reads the CPU timestamp counter (~0.3ns ticks, invariant on modern
CPUs); ticks convert to ns via a 200ms calibration against the wall clock. The
rdtsc pair costs ~5–10ns, INCLUDED in per-op numbers; rdtsc is non-serialising so
single samples can skew a few ns — fine for percentiles over a million samples,
stated in the header. Throughput is measured separately with one clock around the
whole replay so per-op instrumentation can't distort it; a full warmup replay
precedes both modes; the process pins itself to core 0
(`SetThreadAffinityMask`/`sched_setaffinity`).

## The tests as a defense layer

- **Typed semantics suite**: 16 behaviours × 2 engines — maker-price execution,
  FIFO, price-over-time priority, partial fill resting, market remainder discarded,
  cancel/reduce rules, aggressive limit resting after clearing the book, plus
  FastBook's bounds-throw.
- **Differential fuzz**: 25 seeds × 20k ops + one 200k session; per-op equality on
  fills and return values; snapshot + open-order-count equality and both books'
  invariants every 1024 ops; a quantity-conservation sweep (fills + remainder ==
  submitted, per order).
- **CI**: gcc/clang × Release/ASan+UBSan; sanitizer jobs run the whole fuzz, so the
  pool and intrusive-list code is being memory-checked under randomized load on
  every push. The Release job also smoke-runs the benchmark.

## Likely grilling, implementation level

- *Why is the ladder scan "amortised"?* Each ladder slot between two occupied
  levels is walked at most once per time the gap is crossed; on dense books gaps
  are ~1 tick. The LOBSTER measurement shows what happens when they aren't — that's
  the honest counterexample, and the bitmap-summary fix (scan 64 ticks per tzcnt)
  is named.
- *Why uint32 pool indices instead of pointers?* Half the size (more nodes per
  cache line), stable across pool growth (`vector` may reallocate; indices survive,
  pointers wouldn't), and NIL is an explicit sentinel.
- *Why does the free list being LIFO matter?* The most recently freed node is the
  most likely still in cache; LIFO reuse is a free locality win.
- *Why compare full snapshots only every 1024 ops?* Snapshotting is O(book) — doing
  it per-op would make the fuzzer quadratic and shrink achievable stream lengths.
  Per-op fill/return equality already pins behaviour; checkpoints catch state drift.
- *Where exactly would multithreading enter?* Not inside the book. The standard
  shape is a single matching thread fed by an SPSC ring buffer per session, so the
  book itself stays lock-free by having no sharing at all.

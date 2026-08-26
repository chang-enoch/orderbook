# Limit Order Book: baseline vs. cache-optimized

Two matching engines with identical semantics, benchmarked head-to-head on the same
deterministic order tape. One is written the way you'd write it first — ordinary STL
containers, correct and readable. The other is laid out for the cache.

The question the project exists to answer: **how much does careful C++ data-structure and
memory design actually buy you at p99?**

Headline, on an Apple M3 (arm64, clang 21, `-O3 -march=native`, C++20):

| ns/op (batched)  | baseline | + pool alloc | optimized |
| ---------------- | -------- | ------------ | --------- |
| batched timing   | 61.6     | 35.8         | **15.8**  |
| throughput       | 16.1M/s  | 27.2M/s      | **60.8M/s** |
| vs. baseline     | —        | 1.69×        | **3.77×** |

**These ratios are a property of this machine and this allocator, not of the
engines alone.** A run on Linux/x86 with glibc and g++ 13 reported 88.9 → 71.1 →
31.0 ns/op: a 2.72× total rather than 3.77×, with the allocator accounting for
1.21× rather than 1.69×. macOS `libmalloc` is markedly slower than glibc's, so it
flatters the pooled control. Treat the split as **allocator-dependent, somewhere
between a fifth and a half of the log-gap**, and the total as machine-dependent
too. Reproducing on your own hardware is one `make run-bench`.

### Decomposing the win

"Faster because it is cache-friendly" and "faster because it never calls malloc"
are different claims, and one control cannot separate them — a freelist removes
`malloc` *and* clusters the live nodes, so it would take credit for part of the
layout's win. There are therefore two controls, both running the baseline's exact
algorithm and container shapes:

| step | ns/op | ratio | what it isolates |
| ---- | ----- | ----- | ---------------- |
| baseline | 61.6 | — | `std::map` + `std::list` + `malloc` |
| → scattered pool | 36.3 | 1.70× | no `malloc`, addresses left spread out |
| → clustered pool | 35.8 | 1.01× | same allocator, live nodes compacted |
| → optimized | 15.8 | 2.27× | flat levels, slab arena, hierarchical bitmap |

The clustering step is worth about **1%** here, so on this workload the freelist's
locality bonus is negligible and the 1.70× really is allocation cost. That is a
result about *this* tape, though: the live set is ~65k orders and stays cache-
resident whether it is compacted or not, so this control cannot detect a locality
effect that would only appear with a much larger book. See Known gaps.

### What is not measurable here

Per-operation latency on this machine is quantized to a 41.7 ns timer tick, and the
benchmark reports a **null-book control** — the identical timing loop over an engine
that does nothing — so you can see the floor:

| ns, per-operation timing | mean | p50   | p99 | p99.9 |
| ------------------------ | ---- | ----- | --- | ----- |
| null book (harness only) | 13.0 | < 42  | 42  | 42    |
| baseline                 | 58.4 | 42    | 167 | 250   |
| pooled baseline          | 35.1 | 42    | 84  | 125   |
| optimized                | 19.0 | < 42  | 42  | 84    |

The optimized engine's p99 is **42 ns — one tick, identical to the empty harness.**
Per-operation timing cannot resolve it on this hardware. An earlier version of this
README reported that as a "4.0× p99 improvement"; it was a ratio of tick counts, not
a measurement, and it is gone. Where a percentile is genuinely needed, the batched
and throughput tables are the ones that survive a machine with a different tick.

Both engines produce identical trades, and a differential test checks that after
every event.

## Build and run

Either build works. The Makefile needs nothing installed; CMake needs 3.20+.

```bash
make run-test      # unit + differential tests
make asan          # same, under ASan + UBSan
make run-bench     # the tables above
```

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build --output-on-failure
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
```

Benchmark flags: `--events=N --seed=N --reps=N --capacity=N --aggressive=PCT
--add=PCT --cancel=PCT --depth=TICKS`.

## Layout

| Path                        | What it is                                                                 |
| --------------------------- | -------------------------------------------------------------------------- |
| `src/types.h`               | Shared `Price` / `Qty` / `OrderId` / `Side` / `Trade` / `BookConfig`       |
| `src/orderBook.h` | Baseline, templated on the allocator: `OrderBook`, `PooledOrderBook`, `ScatteredOrderBook` |
| `src/poolAllocator.h` | Clustered and scattered freelist allocators — the two attribution controls |
| `src/fastOrderBook.{h,cpp}` | Flat tick-indexed levels, hierarchical bitmap, slab arena, open addressing |
| `bench/flow.h`              | Deterministic order-flow generator                                         |
| `bench/histogram.h`         | Preallocated sample storage and percentile reporting                       |
| `bench/benchmark.cpp`       | Templated driver, side-by-side comparison                                  |
| `tests/conformance.cpp`     | Unit matching cases + event-by-event differential                          |
| `.github/workflows/ci.yml` | Build, test, and ASan on Linux (gcc + clang) and macOS |
| `src/main.cpp`              | Small demo that prints the book as it changes                              |

Both engines expose the **same concrete API with no virtual functions** — the benchmark is
templated on the engine type, so dispatch cost never contaminates the measurement.

## Semantics

Both books implement the same rules:

- Price-time priority. Aggressive orders match against the opposite side from the touch
  outward, FIFO within a level, with partial fills.
- `Gtc` rests any unfilled remainder; `Ioc` discards it.
- **Modify:** a quantity decrease at the same price keeps queue priority. A price change or
  a quantity increase is a cancel/replace to the back of the new level's queue, and may cross.
- Every call returns a `Status`. A trade count cannot distinguish "refused" from
  "matched nothing", and without that distinction the two engines disagreed silently
  on five separate cases.
- Both engines enforce the same admission rules from `BookConfig`: price band, tick
  alignment, `maxOrders`, no duplicate ids, no zero quantities, and one reserved id
  (`UINT64_MAX`). The baseline has no structural need for a price band; it enforces
  one anyway so that "identical semantics" is a testable claim.
- **Capacity gates resting, not admission.** An aggressive order reduces the book, so
  a full book is no reason to refuse it the trades it would have made; only the
  remainder is turned away.
- **A refused modify leaves the order untouched.** Validation happens before anything
  is destroyed.

## The two designs

### Baseline (`OrderBook`)

`std::map<Price, PriceLevel>` per side (bids `std::greater`, asks `std::less`) so the touch
is always `begin()`. Each level holds a `std::list<Order>` for FIFO priority and O(1) erase
from the middle. An `unordered_map<OrderId, Locator>` makes cancel/modify O(1)-ish.

This is deliberately _not_ pessimized — it's what a competent engineer writes first. Its cost
is inherent: a red-black node per price level, a heap allocation per resting order, and
pointer chasing on every match step.

### Optimized (`FastOrderBook`)

Four structural changes:

1. **Flat tick-indexed levels.** Prices sit on a fixed band configured at construction.
   All levels live in one contiguous array indexed by `(price - minPrice) / tick`. Price →
   level is an index computation, not a tree descent. Since a crossed book is impossible,
   both sides share one array.
2. **Hierarchical occupancy bitmap.** One bit per tick, plus a summary bit per word, plus a
   summary of those. Best bid/ask come from cached hints and are only rescanned when the
   touch itself empties.
3. **Slab arena with intrusive lists.** All orders live in one preallocated vector of 32-byte
   nodes (two per cache line), linked by `uint32_t` handles rather than pointers, with an
   index freelist. **Nothing allocates on the hot path.**
4. **Open-addressed id table.** `OrderId → handle` by linear probing with a splitmix64
   finalizer, keys and values in separate arrays so a probe touches half the memory.
   Erase uses backward-shift deletion, not tombstones.

The cost is a fixed price band — orders outside it are rejected. Real venues have price bands
for the same reason.

## Findings

The four structural changes were the easy part. Everything interesting came from problems
that only showed up under measurement — two of which made the "optimized" engine _slower_
than the one it was replacing, and one of which showed that half the remaining win was not
what it looked like.

The tail figures in findings 1 and 2 are per-operation percentiles, so they carry the
resolution caveat from the top of this file: the large numbers (thousands of ns) span many
ticks and are solid, while any figure at 42 ns is at the harness floor and should be read as
"below one tick".

### 1. A flat bitmap is a trap at the touch

Best-bid/offer tracking with one bit per tick is fine until the touch level empties and the
next occupied level is far away — then the rescan walks every intervening word, which across
a million-tick band is thousands of loads landing squarely in the tail. Under an
all-aggressive tape the optimized book was **12× slower than the baseline at p99**, because
`std::map` gets its new best price from `begin()` for free.

The three-level bitmap turns any gap into roughly three loads.

|                            | p99      |
| -------------------------- | -------- |
| flat bitmap                | 2,125 ns |
| hierarchical bitmap        | 125 ns   |
| _baseline, for comparison_ | _167 ns_ |

To be precise about the mechanism, since it is easy to state loosely: the pathological case
is a side going **entirely empty**, where the scan runs to the end of a million-tick bitmap.
A large gap between two occupied levels costs the same way, but the default tape keeps orders
within 64 ticks of the mid and so rarely produces one — which is why this only surfaced under
an all-aggressive tape, and why the benign tape is listed under Known gaps.

### 2. Tombstones quietly destroy an open-addressed table

Erasing by tombstoning is textbook, and it looked fine at 1.5M events. At 4M it collapsed.
An order feed inserts far more orders over a session than the table has slots, so eventually
_every_ slot has been written at least once, no probe ever terminates on an empty slot again,
and lookups decay toward a full-table scan.

Backward-shift deletion (Knuth 6.4 algorithm R) keeps each cluster a contiguous probe chain
and needs no tombstone at all.

| add            | mean    | p99                  |
| -------------- | ------- | -------------------- |
| tombstones     | 85.8 ns | 791 ns               |
| backward-shift | 18.6 ns | 42 ns (harness floor) |

The mean is the load-bearing number in that table. The p99 improvement is real but its
magnitude is not measurable here — 42 ns is what the empty harness reports.

### 3. Roughly half the win is the allocator, not the layout

The baseline heap-allocates a list node per resting order and a red-black node per
price level; the optimized book preallocates everything. That confounds two claims
— "laid out for the cache" and "stopped calling malloc" — and only a control
separates them.

`PooledOrderBook` is the control: `BasicOrderBook` instantiated over a freelist
allocator instead of `std::allocator`. Same code, same containers, same algorithm.

| | ns/op (batched) | throughput |
|---|---|---|
| baseline | 60.0 | 16.6M/s |
| + pool allocator only | 36.3 | 27.1M/s |
| + flat/arena/bitmap layout | 15.7 | 60.6M/s |

Pooling alone buys 1.64×. The layout buys a further 2.23× on top of it. Both are
real, and the honest summary is "half allocator, half layout" rather than the
cache-first story the earlier version of this README told.

### 4. Over-provisioning capacity costs a third of the throughput

The flat design pays for its speed in sizing discipline. Declaring capacity for 1M live
orders when only ~65k are ever resting spreads the id table across 32MB, and every probe
becomes a TLB and cache miss. The baseline barely notices — its cost is dominated by
pointer-chasing either way — but the optimized engine loses a quarter of its throughput and
most of its p99 advantage.

| `maxOrders` | throughput  | p99    |
| ----------- | ----------- | ------ |
| 2^20        | 22.8M ops/s | 125 ns |
| 2^17        | 30.2M ops/s | 42 ns*  |

Size `BookConfig::maxOrders` to the real live-order count, not to a comfortable upper bound.

\* 42 ns is the harness floor — see the null-book row above. The throughput
column is the load-bearing half of that table.

### The through-line

Every one of these was a **tail** problem, invisible in the mean and invisible in a small
run. Two of them made the rewrite worse than the code it replaced, and none would have been
caught by a throughput benchmark or by a correctness test — only by measuring the whole
distribution, at scale, against a reference implementation known to be right.

## On the measurements

Two things about the harness are worth knowing before trusting any number here.

**Timer resolution, and the null-book control.** Userspace timing on Apple Silicon runs off a
24MHz counter, so every per-operation sample is a multiple of ~41.7 ns, and nothing finer is
available without kernel support. Rather than assert that this is fine, the benchmark measures
it: a null book with the same interface and no work runs through the identical timing loop, and
its distribution is printed as the first row of the latency table. It reports mean 13 ns and
p99 42 ns — so any engine row at 42 ns is reporting the harness, not itself.

That is why the headline numbers come from two other measurements. **Batched timing** puts 64
operations inside one clock pair, so the per-operation cost is derived from a span many ticks
long; it cannot show a tail, but its central estimate does not sit on the floor. **Untimed
throughput** removes the instrumentation entirely. Both are reported for every engine, both
carry the null control, and both are the numbers to use when comparing against a machine with
a different tick.

**Order flow is generated against a scratch book.** The generator drives a real baseline book
during generation so it knows exactly which orders are still resting. Without that, most
cancels and modifies name orders that were already filled, and the benchmark measures failed
hash lookups instead of book work — the target-found rate was 36% naively and is 100% now.
Generation happens entirely ahead of the timed run.

**Everything is preallocated.** The event vector, the trade sink, and the sample storage are
all allocated before timing starts. Nothing allocates, sorts, or prints inside a timed region.

**Engines are interleaved, not run in blocks.** Each repetition runs every engine once, so
thermal and scheduler drift over the life of the process cannot masquerade as a difference
between them. Latency is the median run by p99; throughput is the median of the same number
of repetitions. There is no core pinning — macOS does not offer it — so the far tail still
contains scheduler noise, which is what the max column mostly measures.

## Correctness

`tests/conformance.cpp` is what makes the benchmark mean anything. It runs hand-written cases
against all three engines — FIFO sweep, multi-level sweep, partial rest, IOC remainder,
modify-keeps-priority, modify-loses-priority, reprice-crosses, level reuse — plus the
admission rules that used to diverge silently: id zero, the reserved id, out-of-band prices,
off-tick prices, capacity exhaustion, and a refused modify leaving its order intact.

It then drives every engine over the same tape, comparing **after every single event**:

- the returned `Status` — the decision, not just its consequences
- the full trade sequence — taker, maker, price, quantity, in order
- best bid and best ask, and the live order count

and finally the complete depth on both sides. The tape is generated at tick sizes 1, 5, and
25, with 0–15% of events deliberately made inadmissible, and at a capacity small enough to be
hit under load. Every one of those axes was untested previously, and every one of them had a
divergence hiding in it: the earlier suite passed only because the generator clamped
everything into range and started ids at 1.

A faster book that matches differently is not a faster book.

## Known gaps

- **Per-optimization attribution within the fast book.** The allocator is now
  separated from the layout, and clustering separated from allocation cost, but the
  four structural changes inside the optimized book — flat levels, hierarchical
  bitmap, slab arena, open addressing — are still measured only as a bundle.
- **The locality control is workload-limited.** The scattered-vs-clustered pool
  differs by ~1% here because the live set stays cache-resident either way. On a
  book large enough to miss L2 the split between "allocator" and "locality" could
  look quite different, and this tape cannot show it.
- **The tape is benign.** Mid random-walks one tick at a time and orders rest within
  64 ticks of it, so the working set is ~128 levels and stays cache-resident. There
  are no gap-ups, halts, wide spreads, or arrival bursts, which means the deep-scan
  case that finding #1 is about is under-exercised in the default workload.
- **No core pinning or frequency control.** macOS offers no thread pinning, this runs
  on a laptop under a desktop OS, and no attempt is made to fix clocks. Engines are
  interleaved rep-by-rep and reported as a median to keep drift from masquerading as
  a difference, but the far tail is still partly the scheduler.
- **The numbers are machine-specific.** Ratios measured here have been reported
  differently on other microarchitectures. The batched and throughput tables are the
  ones worth comparing across machines; the per-operation percentiles are not.
- **One reserved order id.** `UINT64_MAX` is the hash table's empty sentinel and is
  refused by both engines with `Status::ReservedId`. Every other id, including zero,
  is legal.
- **Single-threaded.** No concurrency work. A real deployment would put a lock-free
  SPSC ring in front of a pinned matching thread; the matching core itself stays
  single-threaded either way, which is how real books do it.

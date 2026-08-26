# Limit Order Book: baseline vs. cache-optimized

Two matching engines with identical semantics, benchmarked head-to-head on the same
deterministic order tape. One is written the way you'd write it first — ordinary STL
containers, correct and readable. The other is laid out for the cache.

The question the project exists to answer: **how much does careful C++ data-structure and
memory design actually buy you at p99?**

Headline, measured by CI on ubuntu/glibc/gcc (x86-64, `-O3`, C++20) — Linux is
the realistic deployment target, and these numbers are re-measured on every push
rather than reported from a laptop:

| ns/op (batched) | baseline | + pool alloc | optimized   |
| --------------- | -------- | ------------ | ----------- |
| batched timing  | 71.7     | 62.3         | **28.6**    |
| throughput      | 14.1M/s  | 16.1M/s      | **35.4M/s** |
| vs. baseline    | —        | 1.14×        | **2.52×**   |

**Read the middle column first.** "+ pool alloc" is the baseline's exact algorithm
and container shapes over a freelist instead of `malloc`. It accounts for 1.14× of
the 2.52×, so the great majority of the win here is the memory layout — but that
split is a property of the allocator being replaced, and on a slower `malloc` it
looks very different. See below.

### Decomposing the win

"Faster because it is cache-friendly" and "faster because it never calls malloc"
are different claims, and one control cannot separate them — a freelist removes
`malloc` _and_ clusters the live nodes, so it would take credit for part of the
layout's win. There are therefore two controls, both running the baseline's exact
algorithm and container shapes:

| step             | ns/op | ratio | what it isolates                             |
| ---------------- | ----- | ----- | -------------------------------------------- |
| baseline         | 71.7  | —     | `std::map` + `std::list` + `malloc`          |
| → scattered pool | 63.0  | 1.14× | no `malloc`, addresses left spread out       |
| → clustered pool | 62.3  | 1.01× | same allocator, live nodes compacted         |
| → optimized      | 28.6  | 2.18× | flat levels, slab arena, hierarchical bitmap |

The clustering step is worth about **1%**, and it measures 1.01× on _both_
platforms — so on this workload the freelist's locality bonus is negligible and
the allocator step really is allocation cost. That replication across two
allocators and two microarchitectures is what makes it a result rather than a
coincidence.

It is still a result about _this_ tape, though: the live set is ~65k orders and
stays cache-resident whether it is compacted or not, so this control cannot
detect a locality effect that would only appear with a much larger book. See
Known gaps.

#### The allocator share is not portable

The same code on an Apple M3 (macOS `libmalloc`, clang 21) decomposes differently:

| step             | Linux · glibc | M3 · libmalloc |
| ---------------- | ------------- | -------------- |
| baseline         | 71.7          | 61.6           |
| → scattered pool | 1.14×         | **1.70×**      |
| → clustered pool | 1.01×         | 1.01×          |
| → optimized      | 2.18×         | **2.20×**      |
| **total**        | **2.52×**     | **3.77×**      |

Two things fall out of that. The layout step is 2.18× against 2.20× — it survives a
change of allocator, compiler and instruction set, and it is the finding. The
allocator step does not: `libmalloc` is slow enough that removing it looks like a
major optimization, while against glibc the same change is a minor one. **The total
is therefore 2.5–3.8× depending on whose `malloc` you are replacing**, and any
single-platform figure for it is a local condition rather than a result.

Everything that does not allocate runs about 1.75× faster on the M3 than on the CI
runner; the baseline, which allocates constantly, runs only 1.16× faster. Netting
that out puts `malloc` at ~25 ns/op on macOS against ~8.7 ns/op on glibc, for an
identical number of allocations.

### Why the headline is not a p99

Per-operation timing brackets every call with two clock reads, and on both
platforms that instrumentation costs about as much as the work. The benchmark
measures it directly with a **null-book control** — the identical timing loop over
an engine that does nothing — and prints it as the first row:

| ns, per-operation timing | mean | p50 | p99 | p99.9 |
| ------------------------ | ---- | --- | --- | ----- |
| null book (harness only) | 24.0 | 20  | 31  | 31    |
| baseline                 | 87.9 | 80  | 210 | 311   |
| pooled baseline          | 80.0 | 70  | 181 | 291   |
| optimized                | 57.3 | 50  | 171 | 280   |

Those figures compress the result badly: they put the optimized engine 1.23× ahead
at p99, where the uninstrumented measurements put it at 2.52×. The harness floor is
a third of the signal, and the clock reads disturb the very pipeline being measured.

On the M3 it is worse than compression — it is total. There the timer tick is
41.7 ns, and the null book and the optimized engine **both** report a p99 of 42 ns:
one tick, indistinguishable. An earlier version of this README reported that gap as
a "4.0× p99 improvement"; it was arithmetic on tick counts, and it is gone.

So the headline comes from batched timing (64 operations inside one clock pair) and
untimed throughput. Neither can show a tail, and this project therefore does not
claim one.

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

| Path                        | What it is                                                                                 |
| --------------------------- | ------------------------------------------------------------------------------------------ |
| `src/types.h`               | Shared `Price` / `Qty` / `OrderId` / `Side` / `Trade` / `BookConfig`                       |
| `src/orderBook.h`           | Baseline, templated on the allocator: `OrderBook`, `PooledOrderBook`, `ScatteredOrderBook` |
| `src/poolAllocator.h`       | Clustered and scattered freelist allocators — the two attribution controls                 |
| `src/fastOrderBook.{h,cpp}` | Flat tick-indexed levels, hierarchical bitmap, slab arena, open addressing                 |
| `bench/flow.h`              | Deterministic order-flow generator                                                         |
| `bench/histogram.h`         | Preallocated sample storage and percentile reporting                                       |
| `bench/benchmark.cpp`       | Templated driver, side-by-side comparison                                                  |
| `tests/conformance.cpp`     | Unit matching cases + event-by-event differential                                          |
| `.github/workflows/ci.yml`  | Build, test, and ASan on Linux (gcc + clang) and macOS                                     |
| `src/main.cpp`              | Small demo that prints the book as it changes                                              |

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

| add            | mean    | p99                   |
| -------------- | ------- | --------------------- |
| tombstones     | 85.8 ns | 791 ns                |
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

|                            | ns/op (batched) | throughput |
| -------------------------- | --------------- | ---------- |
| baseline                   | 60.0            | 16.6M/s    |
| + pool allocator only      | 36.3            | 27.1M/s    |
| + flat/arena/bitmap layout | 15.7            | 60.6M/s    |

Pooling alone buys 1.64×. The layout buys a further 2.23× on top of it. Both are
real, and the honest summary is "half allocator, half layout" rather than the
cache-first story the earlier version of this README told.

### 4. Over-provisioning capacity costs a third of the throughput

The flat design pays for its speed in sizing discipline. Declaring capacity for 1M live
orders when only ~65k are ever resting spreads the id table across 32MB, and every probe
becomes a TLB and cache miss. The baseline barely notices — its cost is dominated by
pointer-chasing either way — but the optimized engine loses a quarter of its throughput and
most of its p99 advantage.

| `maxOrders` | throughput  | p99     |
| ----------- | ----------- | ------- |
| 2^20        | 22.8M ops/s | 125 ns  |
| 2^17        | 30.2M ops/s | 42 ns\* |

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

**Timer resolution, and the null-book control.** Per-operation samples are quantized to the
system clock's tick — 20 ns on the CI runner, 41.7 ns on Apple Silicon, where the counter runs
at 24MHz and nothing finer is reachable without kernel support. Rather than assert this is
fine, the benchmark measures it: a null book with the same interface and no work runs through
the identical timing loop, and its distribution is the first row of the latency table. On CI it
reports p99 = 31 ns against the optimized engine's 171 ns; on the M3 it reports p99 = 42 ns
against the optimized engine's 42 ns — the same single tick, which is to say no signal at all.

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

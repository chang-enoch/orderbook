# Limit Order Book: baseline vs. cache-optimized

Two matching engines with identical semantics, benchmarked head-to-head on the same
deterministic order tape. One is written the way you'd write it first — ordinary STL
containers, correct and readable. The other is laid out for the cache.

The question the project exists to answer: **how much does careful C++ data-structure and
memory design actually buy you at p99?**

|            | baseline    | optimized   |          |
| ---------- | ----------- | ----------- | -------- |
| mean       | 58.7 ns     | 18.0 ns     | **3.3×** |
| p99        | 167 ns      | 42 ns       | **4.0×** |
| p99.9      | 209 ns      | 83 ns       | **2.5×** |
| p99.99     | 333 ns      | 125 ns      | **2.7×** |
| throughput† | 16.5M ops/s | 61.9M ops/s | **3.8×** |

Apple M3 (arm64), Apple clang 21, `-O3 -march=native`, C++20. 4M generated events,
400k untimed warm-up, 7 repetitions reporting the median run by p99. Both engines
produce **identical trades**.

† Throughput is measured on a separate pass *without* per-operation
instrumentation, reported at the end of `orderbook_bench`. The latency harness
brackets every operation with two clock reads to build the distribution — on this
machine a clock read costs about as much as a book operation, so the ops/sec that
falls out of the timed loop (13.6 / 30.4 M ops/s) is instrumentation-bound rather
than engine-bound. Both engines pay the same overhead, so the percentile comparison
is unaffected; the throughput comparison needs its own untimed pass.

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
| `src/orderBook.{h,cpp}`     | Baseline: ordered map per side, list per level, hash index                 |
| `src/fastOrderBook.{h,cpp}` | Flat tick-indexed levels, hierarchical bitmap, slab arena, open addressing |
| `bench/flow.h`              | Deterministic order-flow generator                                         |
| `bench/histogram.h`         | Preallocated sample storage and percentile reporting                       |
| `bench/benchmark.cpp`       | Templated driver, side-by-side comparison                                  |
| `tests/conformance.cpp`     | Unit matching cases + event-by-event differential                          |
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
- Duplicate ids and zero quantities are rejected.

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

The four structural changes were the easy part. Everything interesting came from three
problems that only showed up under measurement, each of which made the "optimized" engine
_slower_ than the one it was replacing.

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

### 2. Tombstones quietly destroy an open-addressed table

Erasing by tombstoning is textbook, and it looked fine at 1.5M events. At 4M it collapsed.
An order feed inserts far more orders over a session than the table has slots, so eventually
_every_ slot has been written at least once, no probe ever terminates on an empty slot again,
and lookups decay toward a full-table scan.

Backward-shift deletion (Knuth 6.4 algorithm R) keeps each cluster a contiguous probe chain
and needs no tombstone at all.

| add            | mean    | p99    |
| -------------- | ------- | ------ |
| tombstones     | 85.8 ns | 791 ns |
| backward-shift | 18.6 ns | 42 ns  |

### 3. Over-provisioning capacity costs a third of the throughput

The flat design pays for its speed in sizing discipline. Declaring capacity for 1M live
orders when only ~65k are ever resting spreads the id table across 32MB, and every probe
becomes a TLB and cache miss. The baseline barely notices — its cost is dominated by
pointer-chasing either way — but the optimized engine loses a quarter of its throughput and
most of its p99 advantage.

| `maxOrders` | throughput  | p99    |
| ----------- | ----------- | ------ |
| 2^20        | 22.8M ops/s | 125 ns |
| 2^17        | 30.2M ops/s | 42 ns  |

Size `BookConfig::maxOrders` to the real live-order count, not to a comfortable upper bound.

### The through-line

Every one of these was a **tail** problem, invisible in the mean and invisible in a small
run. Two of them made the rewrite worse than the code it replaced, and none would have been
caught by a throughput benchmark or by a correctness test — only by measuring the whole
distribution, at scale, against a reference implementation known to be right.

## On the measurements

Two things about the harness are worth knowing before trusting any number here.

**Timer resolution.** Userspace timing on Apple Silicon runs off a 24MHz counter, so every
individual sample is quantized to a multiple of ~41.7ns. The tails span many ticks and are
measured properly, and the mean stays honest because the clock is asynchronous to the work so
the quantization error dithers out. But the optimized engine's p50 reads _zero ticks_ — it is
genuinely below what this machine can time per operation. Read its p50 as "under 42ns", not
as a number.

**Order flow is generated against a scratch book.** The generator drives a real baseline book
during generation so it knows exactly which orders are still resting. Without that, most
cancels and modifies name orders that were already filled, and the benchmark measures failed
hash lookups instead of book work — the target-found rate was 36% naively and is 100% now.
Generation happens entirely ahead of the timed run.

**Everything is preallocated.** The event vector, the trade sink, and the sample storage are
all allocated before timing starts. Nothing allocates, sorts, or prints inside a timed region.

## Correctness

`tests/conformance.cpp` is what makes the benchmark mean anything. It runs hand-written
matching cases against both engines (FIFO sweep, multi-level sweep, partial rest, IOC
remainder, modify-keeps-priority, modify-loses-priority, reprice-crosses, level reuse), then
drives both over the same tape across five seeds, comparing **after every single event**:

- the full trade sequence — taker, maker, price, quantity, in order
- best bid and best ask
- the live order count

and finally the complete depth on both sides. A faster book that matches differently is not
a faster book.

## Known gaps

- **Per-optimization attribution.** The results show the finished engine versus the baseline,
  and show how much each defect _cost_, but not how much each of the four structural changes
  individually _contributes_. That would need four compile-time variants measured in isolation.
- **Single-threaded.** No concurrency work. A real deployment would put a lock-free
  SPSC ring in front of a pinned matching thread; the matching core itself stays
  single-threaded either way, which is how real books do it.
- **Slab exhaustion is silent.** `Rest()` drops the order if the arena or table is full rather
  than signalling. Fine for a benchmark, wrong for anything real.
- `orderbook_test` inherits `-O3` from the library target in CMake Release builds. The
  sanitizer build is a separate `Debug` tree, so this only affects stack-trace readability.

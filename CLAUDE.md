# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

A **measurement study**, not a library. Four order-book engines with byte-identical
semantics are benchmarked against each other to answer one question: how much does
cache-friendly C++ actually buy over idiomatic STL, and how much of that is really
just avoiding `malloc`?

That framing drives most of the design decisions below. A change that makes an engine
faster but breaks the comparison is a regression.

## Commands

```bash
make all           # bench + test + demo (needs nothing installed)
make run-test      # unit + differential suite
make asan          # same suite under ASan + UBSan
make run-bench     # the tables in the README
make clean
```

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build --output-on-failure
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
# -DENABLE_NATIVE=OFF drops -march=native
```

`make` honours `CXX=` (CI uses it to build with both g++ and clang++).

### Running a narrower test

There is no per-test filter. `orderbook_test <events>` takes an event count that
scales the differential runs (default 200000); pass something small like `5000` for a
fast loop. Failures print a `[case-name]` tag from `g_case` — grep that string in
`tests/conformance.cpp` to find the block. The six differential configurations are a
`Case{seed, tick, pctInvalid, capacity}` list in `main()`; comment out rows to narrow.

### Benchmark flags

`--events --seed --reps --capacity --batch --tick --invalid --aggressive --add --cancel --depth`

Defaults are chosen so a bare `./build/orderbook_bench` reproduces the README. In
particular `--capacity` defaults to `1<<17`, sized to the live-order count the flow
actually produces — over-declaring it spreads the id table over memory it never uses
and costs most of the optimized engine's advantage.

## Architecture

### Four engines, one semantics

| engine | header | what it is |
| --- | --- | --- |
| `OrderBook` | `src/orderBook.h` | `std::map` per side, `std::list` per level, `unordered_map` index |
| `PooledOrderBook` | same | identical code, freelist allocator, clustered addresses |
| `ScatteredOrderBook` | same | identical code, freelist allocator, shuffled addresses |
| `FastOrderBook` | `src/fastOrderBook.h` | flat tick-indexed levels, hierarchical bitmap, slab arena, open addressing |

The first three are one class: `BasicOrderBook<Alloc>`, templated on the allocator so
the algorithm is provably identical across them. **They exist as controls.** A plain
freelist removes `malloc` *and* clusters live nodes, so the scattered variant isolates
allocation cost from locality. Do not collapse them.

Engines are compared by value, not through a base class — the benchmark is templated
so virtual dispatch never enters the measurement. There is deliberately no interface
type to inherit from.

### The rules every engine must share

Live in `Admissible()` (both engines) and `ValidateConfig()` (`src/types.h`):

- Every operation returns `Status`, never a bool or a trade count. A trade count
  cannot distinguish "refused" from "matched nothing" — that ambiguity is what let
  the engines disagree silently on six separate cases.
- Both engines enforce the *same* `BookConfig`: price band, tick alignment,
  `maxOrders`. The baseline has no structural need for a price band; it enforces one
  anyway so "identical semantics" is testable rather than asserted.
- `kReservedOrderId` (`UINT64_MAX`) is the flat book's empty-slot sentinel and is
  rejected by every engine. Every other id, **including 0**, is legal. Zero used to be
  the sentinel, which made `AddOrder(0,…)` look like a duplicate and `CancelOrder(0)`
  read out of bounds.
- Capacity gates *resting*, not admission. An aggressive order reduces the book and
  is allowed to trade against a full one.
- A refused `ModifyOrder` leaves its order untouched. Validate before destroying.

**If you change any rule, change it in both engines in the same commit.** The
differential test is what catches a miss.

### Non-obvious invariants in `FastOrderBook`

- Bids and asks share **one** level array. Safe only because matching happens before
  resting, so a crossed book is impossible and no tick is ever occupied on both sides.
- The occupancy bitmap is three levels deep (bit per tick, summary per word, summary
  of summaries). A flat bitmap is correct but walks to the end of a million-tick band
  when a side empties.
- The id table uses **backward-shift deletion, not tombstones**. A feed inserts far
  more orders over a session than the table has slots, so tombstones eventually fill
  every slot and probes stop terminating.
- The table must never exceed half full; `AtCapacity()` enforces it, because a full
  table makes the probe loop non-terminating.

## Measurement methodology

This is the part most likely to be broken by a well-meaning change.

- **Per-operation p99 is not a headline number here.** Samples are quantized to the
  clock tick (20 ns on CI, 41.7 ns on Apple Silicon). The benchmark runs a `NullBook`
  — same interface, no work — through the identical timing loop and prints it as the
  first row. On Apple Silicon the null book and the optimized engine both report
  p99 = 42 ns. Any ratio built from those is arithmetic on tick counts.
- Headline numbers come from **batched timing** (N ops per clock pair) and **untimed
  throughput**. Neither can show a tail; the project does not claim one.
- Every measurement is repeated and **interleaved rep-by-rep** across engines, then
  medianed. Running all of one engine and then all of another lets drift masquerade as
  a difference. Any new measurement pass must follow the same protocol.
- Results are platform-dependent and the README says so. The layout step (~2.2×)
  replicates across allocators and architectures; the allocator step (1.14× glibc,
  1.70× macOS `libmalloc`) does not. Do not restate the total as a single figure.

### Order flow

`bench/flow.h` generates the tape ahead of the timed run, driving a scratch
`OrderBook` so it knows exactly which orders are still resting — otherwise most
cancels name already-filled orders and the benchmark measures failed lookups. It works
in tick-index space so prices are aligned by construction, and `pctInvalid` emits
deliberately inadmissible events so rejection paths are exercised under load.

## Testing

`tests/conformance.cpp` is the thing that makes the benchmark mean anything. It runs
unit cases against all four engines, then drives every engine over the same tape and
compares **after every event**: the returned `Status`, the full trade sequence, the
touch, the live count, and finally the whole depth. It runs at ticks 1/5/25, at 0–15%
inadmissible events, and at a capacity small enough to be hit.

**Adding an engine means adding it to `UnitCases` and to a `DifferentialPair` row.**
An engine that is benchmarked but not differentially tested is worse than no engine.

## Gotchas

- **ASan on macOS hides alloc/dealloc mismatches.** Darwin's runtime disables
  `alloc_dealloc_mismatch` by default and Apple clang has no `-Wmismatched-new-delete`,
  so a `new[]`/`operator delete` mismatch in `poolAllocator.h` went unseen locally
  while GCC flagged it five times. The `asan` target forces the option on; do not
  remove it. Anything allocated with `::operator new(bytes, align_val_t)` must be
  released with the matching aligned `::operator delete`.
- **CI is load-bearing, not decoration.** A single-machine, single-toolchain loop is
  what allowed both the UB above and an allocator-specific conclusion to ship. CI
  builds on ubuntu (gcc + clang) and macOS, and runs the benchmark on glibc so the
  headline numbers are measured rather than reported from a laptop.
- **Editing README tables:** an editor reformats them with alignment padding, so
  string-literal replacements silently no-op. Assert that every replacement matched.
- `src/orderBook.h` is header-only (it is a template). There is no `orderBook.cpp`.

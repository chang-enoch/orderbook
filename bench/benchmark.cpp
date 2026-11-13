#include "fastOrderBook.h"
#include "flow.h"
#include "histogram.h"
#include "orderBook.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using namespace bench;

// Same interface, no work. Running it through the identical timing loop shows
// what the harness alone costs, so a reader can see the floor the engine
// numbers are being compared against rather than taking it on trust.
struct NullBook {
    explicit NullBook(const BookConfig&) {}
    Status AddOrder(OrderId, Side, Price, Qty, TimeInForce, std::vector<Trade>&) {
        return Status::Ok;
    }
    Status CancelOrder(OrderId) { return Status::Ok; }
    Status ModifyOrder(OrderId, Price, Qty, std::vector<Trade>&) { return Status::Ok; }
};

struct RunResult {
    Stats         all;
    Stats         perOp[kOpCount];
    std::uint64_t accepted[kOpCount] = {};
    std::uint64_t total[kOpCount] = {};
    std::uint64_t trades = 0;
};

std::uint64_t ElapsedNs(Clock::time_point t0, Clock::time_point t1) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}

// Runs the first `warmup` events untimed, so every pass starts from a book
// with realistic depth and warm caches.
template <class Book>
void Warm(Book& book, const std::vector<Event>& events, std::size_t warmup,
          std::vector<Trade>& trades) {
    for (std::size_t i = 0; i < warmup && i < events.size(); ++i) {
        Apply(book, events[i], trades);
        trades.clear();
    }
}

// Per-operation timing. Produces the tail distribution, at the cost of two
// clock reads per operation -- see the note on quantization in histogram.h.
template <class Book>
RunResult Run(const std::vector<Event>& events, const BookConfig& cfg,
              std::size_t warmup, Recorder& rec) {
    Book book(cfg);
    std::vector<Trade> trades;
    trades.reserve(1024);

    RunResult r;
    rec.Reset();
    Warm(book, events, warmup, trades);

    for (std::size_t i = warmup; i < events.size(); ++i) {
        const Event& e = events[i];
        const int op = static_cast<int>(e.op);

        const auto t0 = Clock::now();
        const Status s = Apply(book, e, trades);
        const auto t1 = Clock::now();

        DoNotOptimize(s);
        rec.Record(op, ElapsedNs(t0, t1));
        ++r.total[op];
        r.accepted[op] += (s == Status::Ok) ? 1 : 0;
        r.trades += trades.size();
        trades.clear();
    }

    rec.Finalize();
    r.all = rec.SummarizeAll();
    for (int op = 0; op < kOpCount; ++op) r.perOp[op] = rec.Summarize(op);
    return r;
}

// One clock pair per batch of operations, so the per-operation cost is derived
// from a span many ticks long. This is the resolution-independent view: it
// cannot show a tail, but its central estimate does not sit on the timer floor
// the way a 1-tick p50 does.
//
// Statistics over *batch means*, not over individual operations. A "p99" here
// is the 99th-percentile batch, which smooths any tail inside a batch -- it is
// not comparable with the per-operation p99 above, and the column headers say so.
struct BatchStats {
    double meanNs = 0, p50Ns = 0, p99Ns = 0;
};

template <class Book>
BatchStats RunBatched(const std::vector<Event>& events, const BookConfig& cfg,
                      std::size_t warmup, std::size_t batch) {
    Book book(cfg);
    std::vector<Trade> trades;
    trades.reserve(1024);
    Warm(book, events, warmup, trades);

    std::vector<double> perOp;
    perOp.reserve((events.size() - warmup) / batch + 1);
    double sum = 0;

    for (std::size_t i = warmup; i + batch <= events.size(); i += batch) {
        const auto t0 = Clock::now();
        for (std::size_t k = 0; k < batch; ++k) {
            const Status s = Apply(book, events[i + k], trades);
            DoNotOptimize(s);
            trades.clear();
        }
        const auto t1 = Clock::now();
        const double ns = static_cast<double>(ElapsedNs(t0, t1)) /
                          static_cast<double>(batch);
        perOp.push_back(ns);
        sum += ns;
    }

    BatchStats b;
    if (perOp.empty()) return b;
    std::sort(perOp.begin(), perOp.end());
    b.meanNs = sum / static_cast<double>(perOp.size());
    b.p50Ns = perOp[perOp.size() / 2];
    b.p99Ns = perOp[std::min(perOp.size() - 1,
                             static_cast<std::size_t>(0.99 * perOp.size()))];
    return b;
}

// Throughput with no per-operation instrumentation at all.
template <class Book>
double RunUntimed(const std::vector<Event>& events, const BookConfig& cfg,
                  std::size_t warmup, std::size_t& tradesOut) {
    Book book(cfg);
    std::vector<Trade> trades;
    trades.reserve(1024);
    Warm(book, events, warmup, trades);

    std::size_t total = 0;
    const auto t0 = Clock::now();
    for (std::size_t i = warmup; i < events.size(); ++i) {
        const Status s = Apply(book, events[i], trades);
        DoNotOptimize(s);
        total += trades.size();
        trades.clear();
    }
    const auto t1 = Clock::now();
    tradesOut = total;
    return static_cast<double>(events.size() - warmup) /
           std::chrono::duration<double>(t1 - t0).count() / 1e6;
}

// --- reporting --------------------------------------------------------------

void PrintHeader() {
    std::printf("  %-10s %10s %9s %8s %8s %8s %8s %8s %10s\n", "engine", "count",
                "mean", "p50", "p90", "p99", "p99.9", "p99.99", "max");
}

void PrintStats(const char* label, const Stats& s, std::uint64_t tick) {
    std::printf("  %-10s %10llu %9.1f %8llu %8llu %8llu %8llu %8llu %10llu   [%llu-%llu ticks]\n",
                label, (unsigned long long)s.count, s.mean,
                (unsigned long long)s.p50, (unsigned long long)s.p90,
                (unsigned long long)s.p99, (unsigned long long)s.p999,
                (unsigned long long)s.p9999, (unsigned long long)s.max,
                (unsigned long long)(s.p50 / (tick ? tick : 1)),
                (unsigned long long)(s.p9999 / (tick ? tick : 1)));
}

// The median repetition, ranked by `key`. Callers guarantee a non-empty list.
template <class T, class Key>
T MedianBy(std::vector<T> v, Key key) {
    std::sort(v.begin(), v.end(),
              [&](const T& a, const T& b) { return key(a) < key(b); });
    return v[v.size() / 2];
}

const auto byP99    = [](const RunResult& r) { return r.all.p99; };
const auto byMean   = [](const BatchStats& b) { return b.meanNs; };
const auto identity = [](double x) { return x; };

// Matches `--name=value` and stores the value. An unrecognized flag is an
// error in main(): ignoring a typo would run the defaults and print them as if
// they were the configuration that was asked for.
template <class T>
bool Flag(const char* arg, const char* name, T& dst) {
    const std::size_t n = std::strlen(name);
    if (std::strncmp(arg, name, n) != 0 || arg[n] != '=') return false;
    const char* value = arg + n + 1;
    if constexpr (std::is_signed_v<T>) {
        dst = static_cast<T>(std::stoll(value));
    } else {
        // stoull accepts "-5" and wraps it to a huge count; refuse instead.
        if (*value == '-') throw std::invalid_argument(arg);
        dst = static_cast<T>(std::stoull(value));
    }
    return true;
}

constexpr const char* kUsage =
    "usage: orderbook_bench [--events=N] [--seed=N] [--reps=N] [--batch=N]\n"
    "                       [--capacity=N] [--tick=N] [--invalid=PCT]\n"
    "                       [--aggressive=PCT] [--add=PCT] [--cancel=PCT]\n"
    "                       [--depth=TICKS]\n";

}  // namespace

int main(int argc, char** argv) {
    FlowConfig flow;
    int reps = 5;
    std::size_t batch = 64;
    // The capacity every README number was measured at. It is already generous
    // (the default tape peaks near 5k live orders); declaring much more spreads
    // the id table over memory it never uses and costs the optimized engine a
    // quarter of its throughput -- so a bare run must default to this, or it
    // reproduces a different number than the README.
    std::size_t capacity = 1u << 17;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        bool known = false;
        try {
            known = Flag(a, "--events", flow.events) || Flag(a, "--seed", flow.seed) ||
                    Flag(a, "--reps", reps) || Flag(a, "--batch", batch) ||
                    Flag(a, "--capacity", capacity) || Flag(a, "--tick", flow.tick) ||
                    Flag(a, "--invalid", flow.pctInvalid) ||
                    Flag(a, "--aggressive", flow.pctAggressive) ||
                    Flag(a, "--add", flow.pctAdd) || Flag(a, "--cancel", flow.pctCancel) ||
                    Flag(a, "--depth", flow.depthTicks);
        } catch (const std::exception&) {
            std::fprintf(stderr, "bad value: %s\n%s", a, kUsage);
            return 2;
        }
        if (!known) {
            std::fprintf(stderr, "unknown flag: %s\n%s", a, kUsage);
            return 2;
        }
    }
    // Refuse values that would hang or crash rather than measure: batch = 0
    // never advances the batched loop, reps = 0 leaves nothing to take a median
    // of, depth = 0 is a modulo by zero in the generator.
    auto inPct = [](int p) { return p >= 0 && p <= 100; };
    const char* bad = nullptr;
    if (reps < 1 || batch < 1)             bad = "--reps and --batch must be at least 1";
    else if (flow.events < 1)              bad = "--events must be at least 1";
    else if (flow.depthTicks < 1)          bad = "--depth must be at least 1";
    else if (!inPct(flow.pctAdd) || !inPct(flow.pctCancel) ||
             flow.pctAdd + flow.pctCancel > 100 || !inPct(flow.pctAggressive) ||
             !inPct(flow.pctInvalid))
        bad = "percentages must be 0-100, and --add plus --cancel at most 100";

    BookConfig cfg;
    cfg.minPrice = flow.bandLow;
    cfg.maxPrice = flow.bandHigh;
    cfg.tick = flow.tick;
    cfg.maxOrders = capacity;
    if (!bad && ValidateConfig(cfg) != Status::Ok)
        bad = "--tick must be positive and --capacity between 1 and 2^32-2";
    if (bad) {
        std::fprintf(stderr, "%s\n%s", bad, kUsage);
        return 2;
    }

    std::printf("generating %zu events (seed %llu, tick %lld, %d%% invalid)...\n",
                flow.events, (unsigned long long)flow.seed, (long long)flow.tick,
                flow.pctInvalid);
    const std::vector<Event> events = Generate(flow);
    const std::size_t warmup = events.size() / 10;

    const std::uint64_t tick = ClockTickNs();
    std::printf(
        "smallest back-to-back clock delta: %llu ns. Where that is the clock's\n"
        "  resolution (Apple Silicon), samples are multiples of it and a p50 of\n"
        "  one step means \"below %llu ns\"; where the clock is finer (Linux),\n"
        "  it is the cost of reading the clock. Either way the null-book row\n"
        "  below is the harness floor: read the engine rows against it, and\n"
        "  prefer the batched and throughput tables for any comparison that\n"
        "  needs to survive a different machine.\n",
        (unsigned long long)tick, (unsigned long long)tick);
    std::printf("warmup: %zu events (untimed), timed: %zu, reps: %d, capacity: %zu\n\n",
                warmup, events.size() - warmup, reps, capacity);

    // Interleave the engines rep by rep. Running all of one and then all of the
    // other lets thermal and scheduler drift masquerade as a difference between
    // them.
    Recorder rec(events.size());
    std::vector<RunResult> nullRuns, baseRuns, poolRuns, fastRuns;
    for (int i = 0; i < reps; ++i) {
        nullRuns.push_back(Run<NullBook>(events, cfg, warmup, rec));
        baseRuns.push_back(Run<OrderBook>(events, cfg, warmup, rec));
        poolRuns.push_back(Run<PooledOrderBook>(events, cfg, warmup, rec));
        fastRuns.push_back(Run<FastOrderBook>(events, cfg, warmup, rec));
    }

    const RunResult nul  = MedianBy(nullRuns, byP99);
    const RunResult base = MedianBy(baseRuns, byP99);
    const RunResult pool = MedianBy(poolRuns, byP99);
    const RunResult fast = MedianBy(fastRuns, byP99);

    std::printf("=== per-operation latency (ns), median run of %d by p99 ===\n", reps);
    PrintHeader();
    PrintStats("null", nul.all, tick);
    PrintStats("baseline", base.all, tick);
    PrintStats("pooled", pool.all, tick);
    PrintStats("optimized", fast.all, tick);

    std::printf("\n  by operation (optimized vs baseline):\n");
    std::printf("  %-10s %9s %9s %9s %9s\n", "op", "base mean", "base p99",
                "opt mean", "opt p99");
    for (int op = 0; op < kOpCount; ++op)
        std::printf("  %-10s %9.1f %9llu %9.1f %9llu\n", OpName(static_cast<Op>(op)),
                    base.perOp[op].mean, (unsigned long long)base.perOp[op].p99,
                    fast.perOp[op].mean, (unsigned long long)fast.perOp[op].p99);
    std::printf("  accepted:");
    for (int op = 0; op < kOpCount; ++op) {
        if (base.total[op] == 0) continue;
        std::printf("  %s %.1f%%", OpName(static_cast<Op>(op)),
                    100.0 * static_cast<double>(base.accepted[op]) /
                        static_cast<double>(base.total[op]));
    }
    std::printf("\n");

    std::printf("\n=== batched timing, %zu ops per clock pair (ns/op) ===\n", batch);
    std::printf("  not quantized by the timer, so these survive a machine with a\n"
                "  different tick; they cannot show a tail\n");
    std::printf("  %-10s %10s %10s %10s\n", "engine", "mean", "median", "p99");
    std::printf("  %-10s %10s %10s %10s\n", "", "ns/op", "batch", "batch");
    auto batchRow = [&](const char* label, const BatchStats& b) {
        std::printf("  %-10s %10.1f %10.1f %10.1f\n", label, b.meanNs, b.p50Ns,
                    b.p99Ns);
    };
    // Repeated and interleaved, like the latency and throughput passes. Running
    // each engine once, in engine order, is exactly the protocol that lets drift
    // over the life of the process masquerade as a difference between engines.
    std::vector<BatchStats> bn, bb, bp, bs, bf;
    for (int i = 0; i < reps; ++i) {
        bn.push_back(RunBatched<NullBook>(events, cfg, warmup, batch));
        bb.push_back(RunBatched<OrderBook>(events, cfg, warmup, batch));
        bp.push_back(RunBatched<PooledOrderBook>(events, cfg, warmup, batch));
        bs.push_back(RunBatched<ScatteredOrderBook>(events, cfg, warmup, batch));
        bf.push_back(RunBatched<FastOrderBook>(events, cfg, warmup, batch));
    }
    const BatchStats mb = MedianBy(bb, byMean), mp = MedianBy(bp, byMean),
                     ms = MedianBy(bs, byMean), mf = MedianBy(bf, byMean);
    batchRow("null", MedianBy(bn, byMean));
    batchRow("baseline", mb);
    batchRow("scattered", ms);
    batchRow("pooled", mp);
    batchRow("optimized", mf);
    std::printf("\n  allocator vs locality, from the batched means:\n");
    std::printf("    baseline -> scattered  %.2fx   no malloc, malloc-like scatter\n",
                mb.meanNs / ms.meanNs);
    std::printf("    scattered -> pooled    %.2fx   same allocator, clustered addresses\n",
                ms.meanNs / mp.meanNs);
    std::printf("    pooled -> optimized    %.2fx   flat levels, arena, bitmap\n",
                mp.meanNs / mf.meanNs);

    std::printf("\n=== throughput, no per-op instrumentation (median of %d) ===\n",
                reps);
    std::size_t bTrades = 0, pTrades = 0, sTrades = 0, fTrades = 0;
    std::vector<double> bM, pM, sM, fM;
    for (int i = 0; i < reps; ++i) {
        bM.push_back(RunUntimed<OrderBook>(events, cfg, warmup, bTrades));
        sM.push_back(RunUntimed<ScatteredOrderBook>(events, cfg, warmup, sTrades));
        pM.push_back(RunUntimed<PooledOrderBook>(events, cfg, warmup, pTrades));
        fM.push_back(RunUntimed<FastOrderBook>(events, cfg, warmup, fTrades));
    }
    const double bMops = MedianBy(bM, identity), pMops = MedianBy(pM, identity),
                 sMops = MedianBy(sM, identity), fMops = MedianBy(fM, identity);
    std::printf("  %-10s %10.2f M ops/sec\n", "baseline", bMops);
    std::printf("  %-10s %10.2f M ops/sec   %.2fx  no malloc, scattered\n",
                "scattered", sMops, sMops / bMops);
    std::printf("  %-10s %10.2f M ops/sec   %.2fx  no malloc, clustered\n",
                "pooled", pMops, pMops / bMops);
    std::printf("  %-10s %10.2f M ops/sec   %.2fx over baseline, %.2fx over pooled\n",
                "optimized", fMops, fMops / bMops, fMops / pMops);
    std::printf("\n  The middle two rows are the controls that decompose the result.\n"
                "  Both run the baseline's exact algorithm and container shapes over\n"
                "  a freelist, so neither calls malloc; they differ only in whether\n"
                "  the live nodes end up clustered or spread out. baseline ->\n"
                "  scattered is the allocator's share, scattered -> pooled is the\n"
                "  locality a freelist gives away for free, and the rest is layout.\n"
                "\n  This split is a property of THIS allocator and THIS machine.\n"
                "  A run against glibc reports a materially smaller allocator share\n"
                "  than macOS libmalloc does -- see the README.\n");

    // A cheap end-to-end cross-check; the conformance test is the real one.
    // Failing the process matters: CI publishes these numbers, and numbers
    // from engines that disagree are not a comparison.
    const bool agree = base.trades == pool.trades && base.trades == fast.trades &&
                       bTrades == sTrades && bTrades == pTrades && bTrades == fTrades;
    if (!agree) {
        std::printf("\n  ERROR: trade counts differ\n"
                    "    latency pass:    baseline %llu, pooled %llu, optimized %llu\n"
                    "    throughput pass: baseline %zu, scattered %zu, pooled %zu, "
                    "optimized %zu\n",
                    (unsigned long long)base.trades, (unsigned long long)pool.trades,
                    (unsigned long long)fast.trades, bTrades, sTrades, pTrades, fTrades);
        return 1;
    }
    std::printf("\n  all engines produced %llu trades\n", (unsigned long long)base.trades);
    return 0;
}

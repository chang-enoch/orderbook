#include "flow.h"
#include "histogram.h"
#include "orderBook.h"
#ifdef ORDERBOOK_HAS_FAST
#include "fastOrderBook.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
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
    Stats         perOp[3];
    std::uint64_t accepted[3] = {0, 0, 0};
    std::uint64_t total[3] = {0, 0, 0};
    std::uint64_t trades = 0;
};

template <class Book>
Status ApplyTo(Book& b, const Event& e, std::vector<Trade>& trades) {
    switch (e.op) {
        case Op::Add:
            return b.AddOrder(e.id, e.side, e.price, e.qty, e.tif, trades);
        case Op::Cancel:
            return b.CancelOrder(e.id);
        default:
            return b.ModifyOrder(e.id, e.price, e.qty, trades);
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

    for (std::size_t i = 0; i < warmup && i < events.size(); ++i) {
        ApplyTo(book, events[i], trades);
        trades.clear();
    }

    for (std::size_t i = warmup; i < events.size(); ++i) {
        const Event& e = events[i];
        const int op = static_cast<int>(e.op);

        const auto t0 = Clock::now();
        const Status s = ApplyTo(book, e, trades);
        const auto t1 = Clock::now();

        DoNotOptimize(s);
        rec.Record(op, static_cast<std::uint64_t>(
                           std::chrono::duration_cast<std::chrono::nanoseconds>(
                               t1 - t0)
                               .count()));
        ++r.total[op];
        r.accepted[op] += (s == Status::Ok) ? 1 : 0;
        r.trades += trades.size();
        trades.clear();
    }

    rec.Finalize();
    r.all = rec.SummarizeAll();
    for (int op = 0; op < 3; ++op) r.perOp[op] = rec.Summarize(op);
    return r;
}

// One clock pair per batch of operations, so the per-operation cost is derived
// from a span many ticks long. This is the resolution-independent view: it
// cannot show a tail, but its central estimate does not sit on the timer floor
// the way a 1-tick p50 does.
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

    for (std::size_t i = 0; i < warmup && i < events.size(); ++i) {
        ApplyTo(book, events[i], trades);
        trades.clear();
    }

    std::vector<double> perOp;
    perOp.reserve((events.size() - warmup) / batch + 1);
    double sum = 0;

    for (std::size_t i = warmup; i + batch <= events.size(); i += batch) {
        const auto t0 = Clock::now();
        for (std::size_t k = 0; k < batch; ++k) {
            const Status s = ApplyTo(book, events[i + k], trades);
            DoNotOptimize(s);
            trades.clear();
        }
        const auto t1 = Clock::now();
        const double ns =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count()) /
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

    for (std::size_t i = 0; i < warmup && i < events.size(); ++i) {
        ApplyTo(book, events[i], trades);
        trades.clear();
    }

    std::size_t total = 0;
    const auto t0 = Clock::now();
    for (std::size_t i = warmup; i < events.size(); ++i) {
        const Status s = ApplyTo(book, events[i], trades);
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

RunResult MedianByP99(std::vector<RunResult> runs) {
    std::sort(runs.begin(), runs.end(),
              [](const RunResult& a, const RunResult& b) {
                  return a.all.p99 < b.all.p99;
              });
    return runs[runs.size() / 2];
}

double Median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    FlowConfig flow;
    int reps = 5;
    std::size_t batch = 64;
    // Sized to the live-order count the flow actually produces. Over-declaring
    // spreads the id table over memory it never uses and costs most of the
    // advantage -- so the default has to be sane, or a bare run reproduces a
    // different number than the README.
    std::size_t capacity = 1u << 17;

    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--events=", 9) == 0)
            flow.events = std::stoull(argv[i] + 9);
        else if (std::strncmp(argv[i], "--seed=", 7) == 0)
            flow.seed = std::stoull(argv[i] + 7);
        else if (std::strncmp(argv[i], "--reps=", 7) == 0)
            reps = std::stoi(argv[i] + 7);
        else if (std::strncmp(argv[i], "--aggressive=", 13) == 0)
            flow.pctAggressive = std::stoi(argv[i] + 13);
        else if (std::strncmp(argv[i], "--add=", 6) == 0)
            flow.pctAdd = std::stoi(argv[i] + 6);
        else if (std::strncmp(argv[i], "--cancel=", 9) == 0)
            flow.pctCancel = std::stoi(argv[i] + 9);
        else if (std::strncmp(argv[i], "--depth=", 8) == 0)
            flow.depthTicks = std::stoi(argv[i] + 8);
        else if (std::strncmp(argv[i], "--tick=", 7) == 0)
            flow.tick = std::stoll(argv[i] + 7);
        else if (std::strncmp(argv[i], "--invalid=", 10) == 0)
            flow.pctInvalid = std::stoi(argv[i] + 10);
        else if (std::strncmp(argv[i], "--batch=", 8) == 0)
            batch = std::stoull(argv[i] + 8);
        else if (std::strncmp(argv[i], "--capacity=", 11) == 0)
            capacity = std::stoull(argv[i] + 11);
    }

    BookConfig cfg;
    cfg.minPrice = flow.bandLow;
    cfg.maxPrice = flow.bandHigh;
    cfg.tick = flow.tick;
    cfg.maxOrders = capacity;

    std::printf("generating %zu events (seed %llu, tick %lld, %d%% invalid)...\n",
                flow.events, (unsigned long long)flow.seed, (long long)flow.tick,
                flow.pctInvalid);
    const std::vector<Event> events = Generate(flow);
    const std::size_t warmup = events.size() / 10;

    const std::uint64_t tick = ClockTickNs();
    std::printf(
        "timer granularity: %llu ns. Every per-operation sample is a multiple\n"
        "  of this, so a p50 of one tick means \"below %llu ns\", not \"%llu ns\".\n"
        "  The null-book row below is the harness floor: read the engine rows\n"
        "  against it, and prefer the batched and throughput tables for any\n"
        "  comparison that needs to survive a different machine.\n",
        (unsigned long long)tick, (unsigned long long)tick,
        (unsigned long long)tick);
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
#ifdef ORDERBOOK_HAS_FAST
        fastRuns.push_back(Run<FastOrderBook>(events, cfg, warmup, rec));
#endif
    }

    const RunResult nul = MedianByP99(nullRuns);
    const RunResult base = MedianByP99(baseRuns);
    const RunResult pool = MedianByP99(poolRuns);

    std::printf("=== per-operation latency (ns), median run of %d by p99 ===\n", reps);
    PrintHeader();
    PrintStats("null", nul.all, tick);
    PrintStats("baseline", base.all, tick);
    PrintStats("pooled", pool.all, tick);
#ifdef ORDERBOOK_HAS_FAST
    const RunResult fast = MedianByP99(fastRuns);
    PrintStats("optimized", fast.all, tick);
#endif

    std::printf("\n  by operation (optimized vs baseline):\n");
    std::printf("  %-10s %9s %9s %9s %9s\n", "op", "base mean", "base p99",
                "opt mean", "opt p99");
#ifdef ORDERBOOK_HAS_FAST
    for (int op = 0; op < 3; ++op)
        std::printf("  %-10s %9.1f %9llu %9.1f %9llu\n", OpName(static_cast<Op>(op)),
                    base.perOp[op].mean, (unsigned long long)base.perOp[op].p99,
                    fast.perOp[op].mean, (unsigned long long)fast.perOp[op].p99);
#endif
    std::printf("  accepted:");
    for (int op = 0; op < 3; ++op) {
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
#ifdef ORDERBOOK_HAS_FAST
        bf.push_back(RunBatched<FastOrderBook>(events, cfg, warmup, batch));
#endif
    }
    auto medianBatch = [](std::vector<BatchStats> v) {
        std::sort(v.begin(), v.end(), [](const BatchStats& a, const BatchStats& b) {
            return a.meanNs < b.meanNs;
        });
        return v[v.size() / 2];
    };
    const BatchStats mb = medianBatch(bb), mp = medianBatch(bp),
                     ms = medianBatch(bs);
    batchRow("null", medianBatch(bn));
    batchRow("baseline", mb);
    batchRow("scattered", ms);
    batchRow("pooled", mp);
#ifdef ORDERBOOK_HAS_FAST
    const BatchStats mf = medianBatch(bf);
    batchRow("optimized", mf);
    std::printf("\n  allocator vs locality, from the batched means:\n");
    std::printf("    baseline -> scattered  %.2fx   no malloc, malloc-like scatter\n",
                mb.meanNs / ms.meanNs);
    std::printf("    scattered -> pooled    %.2fx   same allocator, clustered addresses\n",
                ms.meanNs / mp.meanNs);
    std::printf("    pooled -> optimized    %.2fx   flat levels, arena, bitmap\n",
                mp.meanNs / mf.meanNs);
#endif

    std::printf("\n=== throughput, no per-op instrumentation (median of %d) ===\n",
                reps);
    std::size_t bTrades = 0, pTrades = 0, sTrades = 0, fTrades = 0;
    std::vector<double> bM, pM, sM, fM;
    for (int i = 0; i < reps; ++i) {
        bM.push_back(RunUntimed<OrderBook>(events, cfg, warmup, bTrades));
        sM.push_back(RunUntimed<ScatteredOrderBook>(events, cfg, warmup, sTrades));
        pM.push_back(RunUntimed<PooledOrderBook>(events, cfg, warmup, pTrades));
#ifdef ORDERBOOK_HAS_FAST
        fM.push_back(RunUntimed<FastOrderBook>(events, cfg, warmup, fTrades));
#endif
    }
    const double bMops = Median(bM), pMops = Median(pM), sMops = Median(sM);
    std::printf("  %-10s %10.2f M ops/sec\n", "baseline", bMops);
    std::printf("  %-10s %10.2f M ops/sec   %.2fx  no malloc, scattered\n",
                "scattered", sMops, sMops / bMops);
    std::printf("  %-10s %10.2f M ops/sec   %.2fx  no malloc, clustered\n",
                "pooled", pMops, pMops / bMops);
#ifdef ORDERBOOK_HAS_FAST
    const double fMops = Median(fM);
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

    if (base.trades != fast.trades || bTrades != fTrades || base.trades != pool.trades)
        std::printf("\n  WARNING: trade counts differ (%llu / %llu / %llu)\n",
                    (unsigned long long)base.trades,
                    (unsigned long long)pool.trades,
                    (unsigned long long)fast.trades);
    else
        std::printf("\n  all engines produced %llu trades\n",
                    (unsigned long long)base.trades);
#endif
    return 0;
}

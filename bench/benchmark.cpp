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

struct RunResult {
    Stats         all;
    Stats         perOp[3];
    std::uint64_t hits[3] = {0, 0, 0};  // ops that found their target
    std::uint64_t total[3] = {0, 0, 0};
    std::uint64_t trades = 0;
    double        elapsedSec = 0;
};

// Drives one engine over the pre-generated event stream. The event vector, the
// trade sink, and the sample storage are all allocated before timing starts, so
// the only work inside a timed region is the book operation itself.
template <class Book>
RunResult Run(const std::vector<Event>& events, const BookConfig& cfg,
              std::size_t warmup, Recorder& rec) {
    Book book(cfg);
    std::vector<Trade> trades;
    trades.reserve(1024);

    RunResult r;
    rec.Reset();

    auto apply = [&](const Event& e) -> bool {
        switch (e.op) {
            case Op::Add:
                book.AddOrder(e.id, e.side, e.price, e.qty, e.tif, trades);
                return true;  // adds use fresh ids, so they always land
            case Op::Cancel:
                return book.CancelOrder(e.id);
            default:
                return book.ModifyOrder(e.id, e.price, e.qty, trades);
        }
    };

    // Warm-up: fills the book to steady-state depth and touches every code
    // path once. Untimed, and its trades are not counted.
    for (std::size_t i = 0; i < warmup && i < events.size(); ++i) {
        apply(events[i]);
        trades.clear();
    }

    const auto wallStart = Clock::now();
    for (std::size_t i = warmup; i < events.size(); ++i) {
        const Event& e = events[i];
        const int op = static_cast<int>(e.op);

        const auto t0 = Clock::now();
        const bool hit = apply(e);
        const auto t1 = Clock::now();

        DoNotOptimize(hit);
        rec.Record(op, static_cast<std::uint64_t>(
                           std::chrono::duration_cast<std::chrono::nanoseconds>(
                               t1 - t0)
                               .count()));
        ++r.total[op];
        r.hits[op] += hit ? 1 : 0;
        r.trades += trades.size();
        trades.clear();
    }
    const auto wallEnd = Clock::now();

    r.elapsedSec = std::chrono::duration<double>(wallEnd - wallStart).count();
    rec.Finalize();
    r.all = rec.SummarizeAll();
    for (int op = 0; op < 3; ++op) r.perOp[op] = rec.Summarize(op);
    return r;
}

// Throughput measured without the per-operation clock reads.
//
// The timed loop above brackets every operation with two Clock::now() calls,
// which is what makes the latency distribution possible -- but on this machine a
// clock read costs about as much as a book operation, so the ops/sec figure that
// comes out of it is instrumentation-bound rather than engine-bound. Both engines
// pay the same overhead, so the percentile comparison is unaffected; the
// throughput comparison is not, and needs its own untimed pass.
template <class Book>
double RunUntimed(const std::vector<Event>& events, const BookConfig& cfg,
                  std::size_t warmup, std::size_t& tradesOut) {
    Book book(cfg);
    std::vector<Trade> trades;
    trades.reserve(1024);

    for (std::size_t i = 0; i < warmup && i < events.size(); ++i) {
        const Event& e = events[i];
        switch (e.op) {
            case Op::Add:
                book.AddOrder(e.id, e.side, e.price, e.qty, e.tif, trades);
                break;
            case Op::Cancel: book.CancelOrder(e.id); break;
            default: book.ModifyOrder(e.id, e.price, e.qty, trades); break;
        }
        trades.clear();
    }

    std::size_t total = 0;
    const auto t0 = Clock::now();
    for (std::size_t i = warmup; i < events.size(); ++i) {
        const Event& e = events[i];
        switch (e.op) {
            case Op::Add:
                book.AddOrder(e.id, e.side, e.price, e.qty, e.tif, trades);
                break;
            case Op::Cancel: book.CancelOrder(e.id); break;
            default: book.ModifyOrder(e.id, e.price, e.qty, trades); break;
        }
        total += trades.size();
        trades.clear();
    }
    const auto t1 = Clock::now();
    tradesOut = total;
    return static_cast<double>(events.size() - warmup) /
           std::chrono::duration<double>(t1 - t0).count() / 1e6;
}

void PrintStats(const char* label, const Stats& s) {
    std::printf("  %-8s %10llu %9.1f %8llu %8llu %8llu %8llu %8llu %10llu\n",
                label, (unsigned long long)s.count, s.mean,
                (unsigned long long)s.p50, (unsigned long long)s.p90,
                (unsigned long long)s.p99, (unsigned long long)s.p999,
                (unsigned long long)s.p9999, (unsigned long long)s.max);
}

void PrintHeader() {
    std::printf("  %-8s %10s %9s %8s %8s %8s %8s %8s %10s\n", "op", "count",
                "mean", "p50", "p90", "p99", "p99.9", "p99.99", "max");
}

void Report(const char* engine, const RunResult& r) {
    std::printf("\n=== %s ===\n", engine);
    PrintHeader();
    for (int op = 0; op < 3; ++op)
        PrintStats(OpName(static_cast<Op>(op)), r.perOp[op]);
    PrintStats("ALL", r.all);
    std::printf("  throughput: %.2f M ops/sec (instrumented)   trades: %llu\n",
                static_cast<double>(r.all.count) / r.elapsedSec / 1e6,
                (unsigned long long)r.trades);
    // Only meaningful for cancel/modify: the generator does not simulate
    // matching, so it can name an order that has already been filled.
    std::printf("  target-found rate:");
    for (int op = 1; op < 3; ++op) {
        if (r.total[op] == 0) continue;
        std::printf("  %s %.1f%%", OpName(static_cast<Op>(op)),
                    100.0 * static_cast<double>(r.hits[op]) /
                        static_cast<double>(r.total[op]));
    }
    std::printf("\n");
}

// Tail latency is noisy; one run is not evidence. Repeat and keep the run whose
// p99 is the median across repetitions.
template <class Book>
RunResult BestOf(const std::vector<Event>& events, const BookConfig& cfg,
                 std::size_t warmup, int reps) {
    Recorder rec(events.size());
    std::vector<RunResult> runs;
    runs.reserve(reps);
    for (int i = 0; i < reps; ++i)
        runs.push_back(Run<Book>(events, cfg, warmup, rec));

    std::sort(runs.begin(), runs.end(),
              [](const RunResult& a, const RunResult& b) {
                  return a.all.p99 < b.all.p99;
              });
    return runs[runs.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    FlowConfig flow;
    int reps = 5;
    std::size_t capacity = 1u << 20;  // declared max live orders
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
        else if (std::strncmp(argv[i], "--capacity=", 11) == 0)
            capacity = std::stoull(argv[i] + 11);
    }

    BookConfig cfg;
    cfg.minPrice  = flow.bandLow;
    cfg.maxPrice  = flow.bandHigh;
    cfg.tick      = flow.tick;
    cfg.maxOrders = capacity;

    std::printf("generating %zu events (seed %llu)...\n", flow.events,
                (unsigned long long)flow.seed);
    const std::vector<Event> events = Generate(flow);
    const std::size_t warmup = events.size() / 10;

    const std::uint64_t tick = ClockTickNs();
    std::printf(
        "timer granularity: %llu ns -- samples are quantized to multiples of\n"
        "  this, so p50/p90 of the cheapest ops are resolution-limited; the\n"
        "  tails and the mean are not.\n",
        (unsigned long long)tick);
    std::printf("warmup: %zu events (untimed), timed: %zu, reps: %d\n", warmup,
                events.size() - warmup, reps);

    const RunResult base =
        BestOf<OrderBook>(events, cfg, warmup, reps);
    Report("baseline (std::map + std::list + unordered_map)", base);

    // Untimed throughput for both engines, so the ops/sec figure is not capped
    // by the latency instrumentation.
    std::size_t baseTrades = 0, fastTrades = 0;
    const double baseMops = RunUntimed<OrderBook>(events, cfg, warmup, baseTrades);
#ifdef ORDERBOOK_HAS_FAST
    const double fastMops =
        RunUntimed<FastOrderBook>(events, cfg, warmup, fastTrades);
#endif

#ifdef ORDERBOOK_HAS_FAST
    const RunResult fast =
        BestOf<FastOrderBook>(events, cfg, warmup, reps);
    Report("optimized (flat levels + bitmap + arena + open addressing)", fast);

    std::printf("\n=== speedup (baseline / optimized) ===\n");
    std::printf("  %-8s %8s %8s %8s %8s %8s\n", "op", "p50", "p90", "p99",
                "p99.9", "p99.99");
    auto ratio = [](std::uint64_t a, std::uint64_t b) {
        return b == 0 ? 0.0 : static_cast<double>(a) / static_cast<double>(b);
    };
    auto row = [&](const char* label, const Stats& b, const Stats& f) {
        std::printf("  %-8s %7.2fx %7.2fx %7.2fx %7.2fx %7.2fx\n", label,
                    ratio(b.p50, f.p50), ratio(b.p90, f.p90),
                    ratio(b.p99, f.p99), ratio(b.p999, f.p999),
                    ratio(b.p9999, f.p9999));
    };
    for (int op = 0; op < 3; ++op)
        row(OpName(static_cast<Op>(op)), base.perOp[op], fast.perOp[op]);
    row("ALL", base.all, fast.all);

    std::printf("\n=== throughput without per-op instrumentation ===\n");
    std::printf("  baseline   %6.2f M ops/sec\n", baseMops);
    std::printf("  optimized  %6.2f M ops/sec   (%.1fx)\n", fastMops,
                fastMops / baseMops);
    if (baseTrades != fastTrades)
        std::printf("  WARNING: untimed trade counts differ (%llu vs %llu)\n",
                    (unsigned long long)baseTrades, (unsigned long long)fastTrades);

    if (base.trades != fast.trades)
        std::printf("\n  WARNING: trade counts differ (%llu vs %llu)\n",
                    (unsigned long long)base.trades,
                    (unsigned long long)fast.trades);
    else
        std::printf("\n  both engines produced %llu trades\n",
                    (unsigned long long)base.trades);
#endif
    return 0;
}

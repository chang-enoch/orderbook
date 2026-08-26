#include "fastOrderBook.h"
#include "flow.h"
#include "orderBook.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
const char* g_case = "";

void Check(bool ok, const std::string& what) {
    if (ok) return;
    std::printf("  FAIL [%s] %s\n", g_case, what.c_str());
    ++g_failures;
}

template <class T>
void CheckEq(const T& got, const T& want, const std::string& what) {
    if (got == want) return;
    std::printf("  FAIL [%s] %s: got %lld, want %lld\n", g_case, what.c_str(),
                (long long)got, (long long)want);
    ++g_failures;
}

std::string TradeStr(const Trade& t) {
    return "taker=" + std::to_string(t.taker) + " maker=" +
           std::to_string(t.maker) + " " + std::to_string(t.qty) + "@" +
           std::to_string(t.price);
}

// ---------------------------------------------------------------------------
// Hand-written semantic cases, run against whichever engine is instantiated.
// ---------------------------------------------------------------------------

template <class Book>
void UnitCases(const char* engine) {
    BookConfig cfg;
    cfg.minPrice = 1;
    cfg.maxPrice = 10'000;
    cfg.maxOrders = 4096;

    {
        g_case = "fifo-sweep";
        Book b(cfg);
        std::vector<Trade> tr;
        b.AddOrder(1, Side::Buy, 100, 50, TimeInForce::Gtc, tr);
        b.AddOrder(2, Side::Buy, 100, 30, TimeInForce::Gtc, tr);
        tr.clear();
        // Takes 50 from order 1 (arrived first), then 10 from order 2.
        b.AddOrder(3, Side::Sell, 100, 60, TimeInForce::Gtc, tr);
        CheckEq(tr.size(), std::size_t{2}, "trade count");
        if (tr.size() == 2) {
            Check(tr[0] == (Trade{3, 1, 100, 50}), "first fill: " + TradeStr(tr[0]));
            Check(tr[1] == (Trade{3, 2, 100, 10}), "second fill: " + TradeStr(tr[1]));
        }
        CheckEq(b.QtyAt(Side::Buy, 100), Qty{20}, "remaining at 100");
        CheckEq(b.BestBid(), Price{100}, "best bid");
    }
    {
        g_case = "multi-level-sweep";
        Book b(cfg);
        std::vector<Trade> tr;
        b.AddOrder(1, Side::Sell, 101, 10, TimeInForce::Gtc, tr);
        b.AddOrder(2, Side::Sell, 102, 10, TimeInForce::Gtc, tr);
        b.AddOrder(3, Side::Sell, 103, 10, TimeInForce::Gtc, tr);
        tr.clear();
        // Sweeps 101 and 102, rests 5 at 103's price limit... but 103 crosses
        // too, so it fills 5 there and rests nothing.
        b.AddOrder(4, Side::Buy, 103, 25, TimeInForce::Gtc, tr);
        CheckEq(tr.size(), std::size_t{3}, "trade count");
        CheckEq(b.BestAsk(), Price{103}, "best ask after sweep");
        CheckEq(b.QtyAt(Side::Sell, 103), Qty{5}, "remaining at 103");
        CheckEq(b.BestBid(), kNoBid, "no bid rests");
    }
    {
        g_case = "partial-rest";
        Book b(cfg);
        std::vector<Trade> tr;
        b.AddOrder(1, Side::Sell, 100, 10, TimeInForce::Gtc, tr);
        tr.clear();
        b.AddOrder(2, Side::Buy, 100, 25, TimeInForce::Gtc, tr);
        CheckEq(tr.size(), std::size_t{1}, "one fill");
        CheckEq(b.QtyAt(Side::Buy, 100), Qty{15}, "remainder rests");
        CheckEq(b.BestAsk(), kNoAsk, "ask side empty");
    }
    {
        g_case = "ioc-discards-remainder";
        Book b(cfg);
        std::vector<Trade> tr;
        b.AddOrder(1, Side::Sell, 100, 10, TimeInForce::Gtc, tr);
        tr.clear();
        b.AddOrder(2, Side::Buy, 100, 25, TimeInForce::Ioc, tr);
        CheckEq(tr.size(), std::size_t{1}, "one fill");
        CheckEq(b.QtyAt(Side::Buy, 100), Qty{0}, "nothing rests");
        CheckEq(b.OrderCount(), std::size_t{0}, "book empty");
    }
    {
        g_case = "no-cross-outside-limit";
        Book b(cfg);
        std::vector<Trade> tr;
        b.AddOrder(1, Side::Sell, 101, 10, TimeInForce::Gtc, tr);
        b.AddOrder(2, Side::Buy, 100, 10, TimeInForce::Gtc, tr);
        CheckEq(tr.size(), std::size_t{0}, "no trades");
        CheckEq(b.BestBid(), Price{100}, "best bid");
        CheckEq(b.BestAsk(), Price{101}, "best ask");
    }
    {
        g_case = "modify-sizedown-keeps-priority";
        Book b(cfg);
        std::vector<Trade> tr;
        b.AddOrder(1, Side::Buy, 100, 50, TimeInForce::Gtc, tr);
        b.AddOrder(2, Side::Buy, 100, 50, TimeInForce::Gtc, tr);
        b.ModifyOrder(1, 100, 20, tr);
        CheckEq(b.QtyAt(Side::Buy, 100), Qty{70}, "level qty after size-down");
        tr.clear();
        b.AddOrder(3, Side::Sell, 100, 30, TimeInForce::Gtc, tr);
        // Order 1 kept its place at the front despite being modified.
        CheckEq(tr.size(), std::size_t{2}, "trade count");
        if (tr.size() == 2) {
            Check(tr[0] == (Trade{3, 1, 100, 20}), "order 1 first: " + TradeStr(tr[0]));
            Check(tr[1] == (Trade{3, 2, 100, 10}), "order 2 second: " + TradeStr(tr[1]));
        }
    }
    {
        g_case = "modify-sizeup-loses-priority";
        Book b(cfg);
        std::vector<Trade> tr;
        b.AddOrder(1, Side::Buy, 100, 10, TimeInForce::Gtc, tr);
        b.AddOrder(2, Side::Buy, 100, 10, TimeInForce::Gtc, tr);
        b.ModifyOrder(1, 100, 30, tr);
        tr.clear();
        b.AddOrder(3, Side::Sell, 100, 15, TimeInForce::Gtc, tr);
        // Order 1 went to the back, so order 2 fills first.
        CheckEq(tr.size(), std::size_t{2}, "trade count");
        if (tr.size() == 2) {
            Check(tr[0] == (Trade{3, 2, 100, 10}), "order 2 first: " + TradeStr(tr[0]));
            Check(tr[1] == (Trade{3, 1, 100, 5}), "order 1 second: " + TradeStr(tr[1]));
        }
    }
    {
        g_case = "modify-reprice-crosses";
        Book b(cfg);
        std::vector<Trade> tr;
        b.AddOrder(1, Side::Sell, 105, 10, TimeInForce::Gtc, tr);
        b.AddOrder(2, Side::Buy, 100, 10, TimeInForce::Gtc, tr);
        tr.clear();
        b.ModifyOrder(2, 105, 10, tr);
        CheckEq(tr.size(), std::size_t{1}, "reprice crossed");
        CheckEq(b.OrderCount(), std::size_t{0}, "both consumed");
    }
    {
        g_case = "cancel";
        Book b(cfg);
        std::vector<Trade> tr;
        b.AddOrder(1, Side::Buy, 100, 10, TimeInForce::Gtc, tr);
        Check(b.CancelOrder(1), "cancel succeeds");
        Check(!b.CancelOrder(1), "double cancel fails");
        Check(!b.CancelOrder(999), "unknown cancel fails");
        CheckEq(b.BestBid(), kNoBid, "book empty");
        CheckEq(b.OrderCount(), std::size_t{0}, "no live orders");
    }
    {
        g_case = "rejects";
        Book b(cfg);
        std::vector<Trade> tr;
        b.AddOrder(1, Side::Buy, 100, 10, TimeInForce::Gtc, tr);
        b.AddOrder(1, Side::Buy, 101, 10, TimeInForce::Gtc, tr);  // duplicate id
        CheckEq(b.OrderCount(), std::size_t{1}, "duplicate id rejected");
        b.AddOrder(2, Side::Buy, 100, 0, TimeInForce::Gtc, tr);  // zero qty
        CheckEq(b.OrderCount(), std::size_t{1}, "zero qty rejected");
        Check(!b.ModifyOrder(999, 100, 5, tr), "modify unknown fails");
    }
    {
        g_case = "level-reuse-after-empty";
        Book b(cfg);
        std::vector<Trade> tr;
        // Repeatedly empty and refill the touch: exercises the best-price
        // rescan and the freelist.
        for (int i = 0; i < 100; ++i) {
            b.AddOrder(1000 + i, Side::Buy, 100 + (i % 7), 10, TimeInForce::Gtc, tr);
            Check(b.CancelOrder(1000 + i), "cancel round " + std::to_string(i));
            CheckEq(b.BestBid(), kNoBid, "empty after round " + std::to_string(i));
        }
        CheckEq(b.OrderCount(), std::size_t{0}, "no leaks");
    }
    std::printf("  unit cases done for %s\n", engine);
}

// ---------------------------------------------------------------------------
// Differential: both engines see the same flow and must agree after every event.
// ---------------------------------------------------------------------------

bool Differential(std::uint64_t seed, std::size_t events) {
    bench::FlowConfig flow;
    flow.seed = seed;
    flow.events = events;
    flow.bandLow = 1;
    flow.bandHigh = 20'000;
    flow.midStart = 10'000;
    flow.depthTicks = 32;

    BookConfig cfg;
    cfg.minPrice = flow.bandLow;
    cfg.maxPrice = flow.bandHigh;
    cfg.tick = flow.tick;
    cfg.maxOrders = 1u << 18;

    const std::vector<bench::Event> stream = bench::Generate(flow);

    OrderBook base(cfg);
    FastOrderBook fast(cfg);
    std::vector<Trade> tb, tf;
    std::vector<std::pair<Price, Qty>> bb, ba, fb, fa;

    for (std::size_t i = 0; i < stream.size(); ++i) {
        const bench::Event& e = stream[i];
        tb.clear();
        tf.clear();

        switch (e.op) {
            case bench::Op::Add:
                base.AddOrder(e.id, e.side, e.price, e.qty, e.tif, tb);
                fast.AddOrder(e.id, e.side, e.price, e.qty, e.tif, tf);
                break;
            case bench::Op::Cancel: {
                const bool rb = base.CancelOrder(e.id);
                const bool rf = fast.CancelOrder(e.id);
                if (rb != rf) {
                    std::printf("  DIFF seed=%llu event %zu: cancel returned %d vs %d\n",
                                (unsigned long long)seed, i, rb, rf);
                    return false;
                }
                break;
            }
            default: {
                const bool rb = base.ModifyOrder(e.id, e.price, e.qty, tb);
                const bool rf = fast.ModifyOrder(e.id, e.price, e.qty, tf);
                if (rb != rf) {
                    std::printf("  DIFF seed=%llu event %zu: modify returned %d vs %d\n",
                                (unsigned long long)seed, i, rb, rf);
                    return false;
                }
                break;
            }
        }

        if (tb.size() != tf.size()) {
            std::printf("  DIFF seed=%llu event %zu: %zu trades vs %zu\n",
                        (unsigned long long)seed, i, tb.size(), tf.size());
            return false;
        }
        for (std::size_t k = 0; k < tb.size(); ++k) {
            if (!(tb[k] == tf[k])) {
                std::printf("  DIFF seed=%llu event %zu trade %zu: %s vs %s\n",
                            (unsigned long long)seed, i, k,
                            TradeStr(tb[k]).c_str(), TradeStr(tf[k]).c_str());
                return false;
            }
        }
        if (base.BestBid() != fast.BestBid() || base.BestAsk() != fast.BestAsk()) {
            std::printf("  DIFF seed=%llu event %zu: touch %lld/%lld vs %lld/%lld\n",
                        (unsigned long long)seed, i, (long long)base.BestBid(),
                        (long long)base.BestAsk(), (long long)fast.BestBid(),
                        (long long)fast.BestAsk());
            return false;
        }
        if (base.OrderCount() != fast.OrderCount()) {
            std::printf("  DIFF seed=%llu event %zu: %zu orders vs %zu\n",
                        (unsigned long long)seed, i, base.OrderCount(),
                        fast.OrderCount());
            return false;
        }
    }

    // Full depth comparison at the end: every occupied level, both sides.
    base.Snapshot(bb, ba);
    fast.Snapshot(fb, fa);
    if (bb != fb || ba != fa) {
        std::printf("  DIFF seed=%llu: final depth differs (%zu/%zu vs %zu/%zu levels)\n",
                    (unsigned long long)seed, bb.size(), ba.size(), fb.size(), fa.size());
        return false;
    }
    std::printf("  seed %-10llu ok  (%zu events, %zu bid / %zu ask levels resting)\n",
                (unsigned long long)seed, events, bb.size(), ba.size());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t events = 200'000;
    if (argc > 1) events = std::stoull(argv[1]);

    std::printf("unit cases:\n");
    UnitCases<OrderBook>("baseline");
    UnitCases<FastOrderBook>("optimized");

    std::printf("\ndifferential (baseline vs optimized):\n");
    for (std::uint64_t seed : {1ull, 7ull, 42ull, 0xC0FFEEull, 123456789ull}) {
        if (!Differential(seed, events)) ++g_failures;
    }

    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}

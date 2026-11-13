#include "fastOrderBook.h"
#include "flow.h"
#include "orderBook.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
const char* g_case = "";

void CheckStatus(Status got, Status want, const std::string& what) {
    if (got == want) return;
    std::printf("  FAIL [%s] %s: got %s, want %s\n", g_case, what.c_str(),
                StatusName(got), StatusName(want));
    ++g_failures;
}

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
        // Sweeps 101 and 102, then takes 5 of the 10 at 103. Nothing rests.
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
        CheckStatus(b.CancelOrder(1), Status::Ok, "cancel succeeds");
        CheckStatus(b.CancelOrder(1), Status::UnknownId, "double cancel");
        CheckStatus(b.CancelOrder(999), Status::UnknownId, "unknown cancel");
        CheckEq(b.BestBid(), kNoBid, "book empty");
        CheckEq(b.OrderCount(), std::size_t{0}, "no live orders");
    }
    {
        g_case = "rejects";
        Book b(cfg);
        std::vector<Trade> tr;
        CheckStatus(b.AddOrder(1, Side::Buy, 100, 10, TimeInForce::Gtc, tr),
                    Status::Ok, "first add");
        CheckStatus(b.AddOrder(1, Side::Buy, 101, 10, TimeInForce::Gtc, tr),
                    Status::DuplicateId, "duplicate id");
        CheckStatus(b.AddOrder(2, Side::Buy, 100, 0, TimeInForce::Gtc, tr),
                    Status::ZeroQty, "zero qty");
        CheckStatus(b.ModifyOrder(999, 100, 5, tr), Status::UnknownId,
                    "modify unknown");
        CheckStatus(b.CancelOrder(999), Status::UnknownId, "cancel unknown");
        CheckEq(b.OrderCount(), std::size_t{1}, "only the valid add landed");
    }
    {
        // Every one of these silently diverged between the two engines before
        // there was a rejection channel to report them through.
        g_case = "admission-rules";
        Book b(cfg);
        std::vector<Trade> tr;

        // Zero is an ordinary id. It used to be the hash table's empty
        // sentinel: the add was refused as a duplicate and the cancel read out
        // of bounds.
        CheckStatus(b.AddOrder(0, Side::Buy, 100, 10, TimeInForce::Gtc, tr),
                    Status::Ok, "id 0 is legal");
        CheckEq(b.QtyAt(Side::Buy, 100), Qty{10}, "id 0 actually rests");
        CheckStatus(b.CancelOrder(0), Status::Ok, "id 0 cancels");
        CheckStatus(b.CancelOrder(0), Status::UnknownId, "id 0 gone");

        CheckStatus(b.AddOrder(kReservedOrderId, Side::Buy, 100, 10,
                               TimeInForce::Gtc, tr),
                    Status::ReservedId, "reserved id refused");
        CheckStatus(b.CancelOrder(kReservedOrderId), Status::ReservedId,
                    "reserved id cancel refused");

        CheckStatus(b.AddOrder(20, Side::Buy, cfg.maxPrice + 1, 10,
                               TimeInForce::Gtc, tr),
                    Status::PriceOutOfBand, "above band");
        CheckStatus(b.AddOrder(21, Side::Buy, cfg.minPrice - 1, 10,
                               TimeInForce::Gtc, tr),
                    Status::PriceOutOfBand, "below band");
        CheckEq(b.OrderCount(), std::size_t{0}, "nothing out-of-band rested");

        // A rejected modify must leave the original untouched. It used to
        // cancel first, fail the re-add, and report success.
        CheckStatus(b.AddOrder(30, Side::Buy, 100, 10, TimeInForce::Gtc, tr),
                    Status::Ok, "order to modify");
        CheckStatus(b.ModifyOrder(30, cfg.maxPrice + 1, 10, tr),
                    Status::PriceOutOfBand, "reprice out of band refused");
        CheckEq(b.OrderCount(), std::size_t{1}, "order survived the refusal");
        CheckEq(b.QtyAt(Side::Buy, 100), Qty{10}, "and kept its price and size");
    }
    {
        g_case = "tick-alignment";
        BookConfig t = cfg;
        t.minPrice = 0;
        t.tick = 5;
        Book b(t);
        std::vector<Trade> tr;
        CheckStatus(b.AddOrder(1, Side::Buy, 100, 10, TimeInForce::Gtc, tr),
                    Status::Ok, "aligned add");
        CheckStatus(b.AddOrder(2, Side::Buy, 102, 10, TimeInForce::Gtc, tr),
                    Status::PriceNotOnTick, "off-tick add");
        CheckStatus(b.ModifyOrder(1, 102, 10, tr), Status::PriceNotOnTick,
                    "off-tick reprice");
        CheckEq(b.OrderCount(), std::size_t{1}, "order survived off-tick reprice");
        CheckEq(b.QtyAt(Side::Buy, 100), Qty{10}, "unchanged");
    }
    {
        // Capacity gates resting, not admission: an aggressive order reduces
        // the book and must still be allowed to trade.
        g_case = "capacity";
        BookConfig t = cfg;
        t.maxOrders = 8;
        Book b(t);
        std::vector<Trade> tr;
        for (int i = 0; i < 8; ++i)
            CheckStatus(b.AddOrder(100 + i, Side::Buy, 100, 1, TimeInForce::Gtc, tr),
                        Status::Ok, "fill to capacity " + std::to_string(i));
        CheckStatus(b.AddOrder(200, Side::Buy, 100, 1, TimeInForce::Gtc, tr),
                    Status::CapacityExhausted, "past capacity");
        CheckEq(b.OrderCount(), std::size_t{8}, "book held at capacity");

        tr.clear();
        CheckStatus(b.AddOrder(300, Side::Sell, 100, 8, TimeInForce::Ioc, tr),
                    Status::Ok, "taker trades despite a full book");
        CheckEq(tr.size(), std::size_t{8}, "all eight filled");
        CheckEq(b.OrderCount(), std::size_t{0}, "book drained");
    }
    {
        g_case = "level-reuse-after-empty";
        Book b(cfg);
        std::vector<Trade> tr;
        // Repeatedly empty and refill the touch: exercises the best-price
        // rescan and the freelist.
        for (int i = 0; i < 100; ++i) {
            b.AddOrder(1000 + i, Side::Buy, 100 + (i % 7), 10, TimeInForce::Gtc, tr);
            CheckStatus(b.CancelOrder(1000 + i), Status::Ok,
                        "cancel round " + std::to_string(i));
            CheckEq(b.BestBid(), kNoBid, "empty after round " + std::to_string(i));
        }
        CheckEq(b.OrderCount(), std::size_t{0}, "no leaks");
    }
    {
        // A modify to qty 0 is a cancel, so the new price is never checked.
        g_case = "modify-to-zero-cancels";
        Book b(cfg);
        std::vector<Trade> tr;
        b.AddOrder(1, Side::Buy, 100, 10, TimeInForce::Gtc, tr);
        CheckStatus(b.ModifyOrder(1, cfg.maxPrice + 1, 0, tr), Status::Ok,
                    "zero-qty modify cancels, even at an out-of-band price");
        CheckEq(b.OrderCount(), std::size_t{0}, "order is gone");
        CheckStatus(b.ModifyOrder(1, 100, 0, tr), Status::UnknownId,
                    "zero-qty modify of an unknown id");
    }
    {
        // Levels scattered across a million-tick band, so best-price rescans
        // have to cross bitmap words at every level of the hierarchy and run
        // off both ends of the band. The differential tape stays within a
        // few dozen ticks of the mid and never reaches these paths.
        g_case = "sparse-wide-band";
        BookConfig w;
        w.minPrice = 1;
        w.maxPrice = 1'000'000;
        w.maxOrders = 4096;
        Book b(w);
        std::vector<Trade> tr;
        const Price bidPx[] = {1, 70'000, 600'000};          // ids 1..3
        const Price askPx[] = {600'001, 999'999, 1'000'000};  // ids 4..6
        OrderId id = 1;
        for (Price p : bidPx) b.AddOrder(id++, Side::Buy, p, 10, TimeInForce::Gtc, tr);
        for (Price p : askPx) b.AddOrder(id++, Side::Sell, p, 10, TimeInForce::Gtc, tr);
        CheckEq(tr.size(), std::size_t{0}, "nothing crosses");

        std::vector<std::pair<Price, Qty>> sb, sa;
        b.Snapshot(sb, sa);
        Check(sb.size() == 3 && sb.front().first == 600'000 && sb.back().first == 1,
              "bid depth spans the band");
        Check(sa.size() == 3 && sa.front().first == 600'001 &&
                  sa.back().first == 1'000'000,
              "ask depth spans the band");

        // Cancel the touch each time: the rescan crosses an l2 word, then an
        // l1 word, then runs off the bottom of the band.
        CheckStatus(b.CancelOrder(3), Status::Ok, "cancel bid 600000");
        CheckEq(b.BestBid(), Price{70'000}, "rescan across l2 words");
        CheckStatus(b.CancelOrder(2), Status::Ok, "cancel bid 70000");
        CheckEq(b.BestBid(), Price{1}, "rescan down to the lowest tick");
        CheckStatus(b.CancelOrder(1), Status::Ok, "cancel bid 1");
        CheckEq(b.BestBid(), kNoBid, "bid side empty");
        CheckStatus(b.CancelOrder(4), Status::Ok, "cancel ask 600001");
        CheckEq(b.BestAsk(), Price{999'999}, "rescan up across l2 words");

        // One aggressive order sweeps to the top of the band...
        tr.clear();
        CheckStatus(b.AddOrder(10, Side::Buy, 1'000'000, 20, TimeInForce::Ioc, tr),
                    Status::Ok, "sweep up");
        CheckEq(tr.size(), std::size_t{2}, "took both remaining asks");
        CheckEq(b.BestAsk(), kNoAsk, "ask side empty");

        // ...and another sweeps down to the bottom of it.
        for (Price p : bidPx) b.AddOrder(id++, Side::Buy, p, 10, TimeInForce::Gtc, tr);
        tr.clear();
        CheckStatus(b.AddOrder(20, Side::Sell, 1, 30, TimeInForce::Ioc, tr),
                    Status::Ok, "sweep down");
        CheckEq(tr.size(), std::size_t{3}, "took all three bids");
        CheckEq(b.BestBid(), kNoBid, "bid side empty after sweep");
        CheckEq(b.OrderCount(), std::size_t{0}, "book empty");
    }
    std::printf("  unit cases done for %s\n", engine);
}

// ---------------------------------------------------------------------------
// Differential: both engines see the same flow and must agree after every event.
// ---------------------------------------------------------------------------

// Compares one engine against the reference after every event: the decision,
// the trades, the touch, the live count, and finally the whole depth.
// `capacityHits` receives how many events the reference refused for capacity,
// so a row meant to exercise that path can prove it did.
template <class Ref, class Test>
bool DifferentialPair(const char* label, const std::vector<bench::Event>& stream,
                      const BookConfig& cfg, std::uint64_t seed,
                      std::size_t& capacityHits) {
    Ref  ref(cfg);
    Test test(cfg);
    std::vector<Trade> tr, tt;
    std::vector<std::pair<Price, Qty>> rb, ra, tb, ta;
    std::size_t rejected = 0;
    capacityHits = 0;

    for (std::size_t i = 0; i < stream.size(); ++i) {
        const bench::Event& e = stream[i];
        tr.clear();
        tt.clear();

        const Status sr = bench::Apply(ref, e, tr);
        const Status st = bench::Apply(test, e, tt);
        rejected += (sr != Status::Ok) ? 1 : 0;
        capacityHits += (sr == Status::CapacityExhausted) ? 1 : 0;

        if (sr != st) {
            std::printf("  DIFF %s seed=%llu event %zu: status %s vs %s\n", label,
                        (unsigned long long)seed, i, StatusName(sr), StatusName(st));
            return false;
        }
        if (tr.size() != tt.size()) {
            std::printf("  DIFF %s seed=%llu event %zu: %zu trades vs %zu\n", label,
                        (unsigned long long)seed, i, tr.size(), tt.size());
            return false;
        }
        for (std::size_t k = 0; k < tr.size(); ++k) {
            if (!(tr[k] == tt[k])) {
                std::printf("  DIFF %s seed=%llu event %zu trade %zu: %s vs %s\n",
                            label, (unsigned long long)seed, i, k,
                            TradeStr(tr[k]).c_str(), TradeStr(tt[k]).c_str());
                return false;
            }
        }
        if (ref.BestBid() != test.BestBid() || ref.BestAsk() != test.BestAsk()) {
            std::printf("  DIFF %s seed=%llu event %zu: touch %lld/%lld vs %lld/%lld\n",
                        label, (unsigned long long)seed, i, (long long)ref.BestBid(),
                        (long long)ref.BestAsk(), (long long)test.BestBid(),
                        (long long)test.BestAsk());
            return false;
        }
        if (ref.OrderCount() != test.OrderCount()) {
            std::printf("  DIFF %s seed=%llu event %zu: %zu orders vs %zu\n", label,
                        (unsigned long long)seed, i, ref.OrderCount(),
                        test.OrderCount());
            return false;
        }
    }

    ref.Snapshot(rb, ra);
    test.Snapshot(tb, ta);
    if (rb != tb || ra != ta) {
        std::printf("  DIFF %s seed=%llu: final depth differs\n", label,
                    (unsigned long long)seed);
        return false;
    }
    std::printf("    %-22s ok  (%zu events, %zu refused, %zu at capacity, %zu/%zu levels)\n",
                label, stream.size(), rejected, capacityHits, rb.size(), ra.size());
    return true;
}

// Admission rules that stop at the constructor are still admission rules. Each
// of these configs used to be accepted by one engine and refused -- or fatally
// mishandled -- by the other.
void ConfigCases() {
    g_case = "config-validation";
    const BookConfig good{1, 1000, 1, 4096};
    CheckStatus(ValidateConfig(good), Status::Ok, "a sane config validates");

    constexpr Price kMin = std::numeric_limits<Price>::min();
    constexpr Price kMax = std::numeric_limits<Price>::max();
    struct Bad { BookConfig cfg; const char* why; };
    const Bad bad[] = {
        {{1, 1000, 1, 0}, "maxOrders = 0"},
        {{1000, 1, 1, 64}, "minPrice > maxPrice"},
        {{1, 1000, 0, 64}, "tick = 0"},
        {{1, 1000, -5, 64}, "negative tick"},
        // Would wrap the optimized book's 32-bit level index or order handle.
        {{0, Price{1} << 32, 1, 64}, "band wider than 2^32 ticks"},
        {{1, 1000, 1, std::size_t{1} << 32}, "maxOrders >= 2^32"},
        // Would overflow `price - minPrice`, even though the tick keeps the
        // level count small.
        {{kMin, kMax, Price{1} << 62, 64}, "band wider than INT64_MAX"},
    };
    // Every engine must refuse it the same way -- silently coercing it, as
    // tick = 0 used to be coerced to 1, is how configs diverge unnoticed.
    auto throws = [](auto construct) {
        // Only the deliberate refusal counts. A bad_alloc from sizing a
        // gigantic band would also "throw", and would hide lost validation.
        try { construct(); } catch (const std::invalid_argument&) { return true; }
        return false;
    };
    for (const Bad& b : bad) {
        CheckStatus(ValidateConfig(b.cfg), Status::InvalidConfig, b.why);
        const bool all = throws([&] { OrderBook x(b.cfg); }) &&
                         throws([&] { PooledOrderBook x(b.cfg); }) &&
                         throws([&] { ScatteredOrderBook x(b.cfg); }) &&
                         throws([&] { FastOrderBook x(b.cfg); });
        Check(all, std::string("all engines reject ") + b.why);
    }
    std::printf("  config cases done\n");
}

bool Differential(std::uint64_t seed, std::size_t events, Price tick,
                  int pctInvalid, std::size_t maxOrders, bool mustHitCapacity) {
    bench::FlowConfig flow;
    flow.seed = seed;
    flow.events = events;
    flow.bandLow = 1;
    flow.bandHigh = 20'000;
    flow.midStart = 10'000;
    flow.depthTicks = 32;
    flow.tick = tick;
    flow.pctInvalid = pctInvalid;

    BookConfig cfg;
    cfg.minPrice = flow.bandLow;
    cfg.maxPrice = flow.bandHigh;
    cfg.tick = tick;
    cfg.maxOrders = maxOrders;

    const std::vector<bench::Event> stream = bench::Generate(flow);
    std::printf("  seed %llu, tick %lld, %d%% invalid, capacity %zu\n",
                (unsigned long long)seed, (long long)tick, pctInvalid, maxOrders);

    bool ok = true;
    std::size_t capacityHits = 0;
    // The pooled baseline runs the identical algorithm over a different
    // allocator, so any disagreement there is an allocator bug, not a logic one.
    ok &= DifferentialPair<OrderBook, PooledOrderBook>("pooled vs baseline",
                                                       stream, cfg, seed, capacityHits);
    ok &= DifferentialPair<OrderBook, ScatteredOrderBook>("scattered vs baseline",
                                                          stream, cfg, seed, capacityHits);
    ok &= DifferentialPair<OrderBook, FastOrderBook>("fast vs baseline", stream,
                                                     cfg, seed, capacityHits);
    // A capacity row that never reaches capacity tests nothing. This one used
    // to be sized above the flow's peak live count and silently did just that.
    if (mustHitCapacity && capacityHits == 0) {
        std::printf("  FAIL capacity %zu was never reached; lower it\n", maxOrders);
        ok = false;
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t events = 200'000;
    if (argc > 1) events = std::stoull(argv[1]);

    std::printf("unit cases:\n");
    UnitCases<OrderBook>("baseline");
    UnitCases<PooledOrderBook>("pooled");
    UnitCases<ScatteredOrderBook>("scattered");
    UnitCases<FastOrderBook>("optimized");
    ConfigCases();

    std::printf("\ndifferential:\n");
    struct Case {
        std::uint64_t seed;
        Price         tick;
        int           invalid;
        std::size_t   cap;
        bool          mustHitCap;
    };
    // Non-unit ticks, deliberately inadmissible events, and a capacity small
    // enough to be hit under load were all untested before, and all three had
    // divergences hiding in them. This flow peaks around 800 live orders, so
    // the last row's 256 is reached constantly.
    for (const Case& c : {Case{1, 1, 0, 1u << 18, false},
                          Case{7, 1, 5, 1u << 18, false},
                          Case{42, 1, 15, 1u << 18, false},
                          Case{0xC0FFEE, 5, 5, 1u << 18, false},
                          Case{123456789, 25, 10, 1u << 18, false},
                          Case{31337, 1, 5, 256, true}}) {
        if (!Differential(c.seed, events / 2, c.tick, c.invalid, c.cap, c.mustHitCap))
            ++g_failures;
    }

    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}

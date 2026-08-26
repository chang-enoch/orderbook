#pragma once

#include "orderBook.h"
#include "types.h"

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace bench {

enum class Op : std::uint8_t { Add, Cancel, Modify };

inline const char* OpName(Op op) {
    switch (op) {
        case Op::Add:    return "add";
        case Op::Cancel: return "cancel";
        case Op::Modify: return "modify";
        default:         return "?";
    }
}

struct Event {
    OrderId     id;
    Price       price;
    Qty         qty;
    Op          op;
    Side        side;
    TimeInForce tif;
};

// xorshift128+: cheap, deterministic, and good enough for order flow. The
// generator runs entirely ahead of time -- no RNG cost ever lands inside a
// timed region.
class Rng {
public:
    explicit Rng(std::uint64_t seed) {
        s_[0] = Mix(seed + 0x9E3779B97F4A7C15ull);
        s_[1] = Mix(seed + 0xBF58476D1CE4E5B9ull);
    }

    std::uint64_t Next() {
        std::uint64_t x = s_[0];
        const std::uint64_t y = s_[1];
        s_[0] = y;
        x ^= x << 23;
        s_[1] = x ^ y ^ (x >> 17) ^ (y >> 26);
        return s_[1] + y;
    }

    // Unbiased enough for benchmark flow; the modulo skew is negligible here.
    std::uint64_t Below(std::uint64_t n) { return Next() % n; }

private:
    static std::uint64_t Mix(std::uint64_t z) {
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    std::uint64_t s_[2];
};

struct FlowConfig {
    std::uint64_t seed        = 0xC0FFEE;
    std::size_t   events      = 2'000'000;
    Price         midStart    = 500'000;
    Price         tick        = 1;
    Price         bandLow     = 1;
    Price         bandHigh    = 1'000'000;
    int           depthTicks  = 64;    // passive orders rest within this many ticks
    int           pctAdd      = 55;    // remainder split cancel/modify below
    int           pctCancel   = 30;
    int           pctAggressive = 22;  // share of adds that cross the spread
    int           pctIoc      = 15;    // share of aggressive adds that are IOC
    // Share of events deliberately made inadmissible. The generator used to
    // clamp everything into range, which is precisely why the two engines were
    // able to disagree on rejection for so long without a test noticing.
    int           pctInvalid  = 0;
    Qty           qtyMin      = 1;
    Qty           qtyMax      = 100;
    int           pctResize   = 70;    // share of modifies that keep the price
    std::size_t   liveRing    = 1u << 16;  // pool of orders cancel/modify draw from
};

// Generates a deterministic stream of add/cancel/modify events around a slowly
// random-walking mid, so the book keeps realistic depth and the touch moves.
//
// The generator does not simulate matching, so a cancel or modify may name an
// order that has already been filled. That is realistic (real venues see the
// same race) and, more importantly, both engines see the identical stream --
// the harness reports the hit rate so the numbers stay interpretable.
inline std::vector<Event> Generate(const FlowConfig& cfg) {
    Rng rng(cfg.seed);
    std::vector<Event> events;
    events.reserve(cfg.events);

    const Price tick = cfg.tick == 0 ? 1 : cfg.tick;
    // Work in tick indices and convert on the way out, so every generated price
    // is tick-aligned by construction and a non-unit tick is exercised properly
    // rather than accidentally.
    const std::int64_t maxIdx = (cfg.bandHigh - cfg.bandLow) / tick;
    auto toPrice = [&](std::int64_t idx) {
        if (idx < 0) idx = 0;
        if (idx > maxIdx) idx = maxIdx;
        return cfg.bandLow + idx * tick;
    };

    // Generation drives a scratch baseline book so it knows exactly which
    // orders are still resting. That costs nothing (it happens entirely ahead
    // of the timed run) and it means cancel/modify events name orders that are
    // really there -- otherwise most of them would degrade into a failed
    // lookup and the benchmark would measure lookup misses, not book work.
    BookConfig bookCfg;
    bookCfg.minPrice = cfg.bandLow;
    bookCfg.maxPrice = cfg.bandHigh;
    bookCfg.tick     = tick;
    bookCfg.maxOrders = cfg.liveRing * 4;
    OrderBook sim(bookCfg);
    std::vector<Trade> trades;
    trades.reserve(256);

    struct LiveOrder { OrderId id; Price price; Qty qty; };
    std::vector<LiveOrder> live;
    live.reserve(cfg.liveRing);
    std::unordered_map<OrderId, std::size_t> slotOf;
    slotOf.reserve(cfg.liveRing * 2);

    auto poolAdd = [&](OrderId id, Price price, Qty qty) {
        if (live.size() >= cfg.liveRing) return;
        slotOf.emplace(id, live.size());
        live.push_back(LiveOrder{id, price, qty});
    };
    auto poolRemove = [&](OrderId id) {
        auto it = slotOf.find(id);
        if (it == slotOf.end()) return;
        const std::size_t slot = it->second;
        slotOf.erase(it);
        live[slot] = live.back();
        if (slot + 1 != live.size()) slotOf[live[slot].id] = slot;
        live.pop_back();
    };
    auto settle = [&](OrderId taker) {
        Qty takerFilled = 0;
        for (const Trade& t : trades) {
            takerFilled += (t.taker == taker) ? t.qty : 0;
            auto it = slotOf.find(t.maker);
            if (it == slotOf.end()) continue;
            LiveOrder& m = live[it->second];
            m.qty -= t.qty;
            if (m.qty == 0) poolRemove(t.maker);
        }
        trades.clear();
        return takerFilled;
    };

    std::int64_t midIdx = (cfg.midStart - cfg.bandLow) / tick;
    OrderId      nextId = 1;

    auto randQty = [&] {
        return cfg.qtyMin + rng.Below(cfg.qtyMax - cfg.qtyMin + 1);
    };
    auto clampIdx = [&](std::int64_t i) {
        return i < 0 ? 0 : (i > maxIdx ? maxIdx : i);
    };

    // Turns a well-formed event into one the book must refuse. Each variant
    // maps onto a distinct Status, so the differential test covers every
    // rejection path rather than only the happy one.
    auto corrupt = [&](Event& e) {
        switch (rng.Below(4)) {
            case 0: e.price = cfg.bandHigh + tick * 10; break;   // out of band
            case 1: e.price = cfg.bandLow - tick * 10; break;    // out of band
            case 2:
                if (tick != 1) e.price += 1;                     // off tick
                else e.qty = 0;                                  // zero qty
                break;
            default: e.id = kReservedOrderId; break;             // reserved id
        }
    };

    for (std::size_t i = 0; i < cfg.events; ++i) {
        if (rng.Below(16) == 0) midIdx = clampIdx(midIdx + (rng.Below(2) ? 1 : -1));

        const std::uint64_t roll = rng.Below(100);
        Event e{};

        if (roll < static_cast<std::uint64_t>(cfg.pctAdd) || live.empty()) {
            const bool buy = rng.Below(2) == 0;
            const bool aggressive =
                rng.Below(100) < static_cast<std::uint64_t>(cfg.pctAggressive);
            const std::int64_t off =
                1 + static_cast<std::int64_t>(rng.Below(cfg.depthTicks));

            e.op   = Op::Add;
            e.id   = nextId++;
            e.side = buy ? Side::Buy : Side::Sell;
            e.qty  = randQty();
            e.price = toPrice(clampIdx(aggressive ? (buy ? midIdx + off : midIdx - off)
                                                  : (buy ? midIdx - off : midIdx + off)));
            e.tif = (aggressive &&
                     rng.Below(100) < static_cast<std::uint64_t>(cfg.pctIoc))
                        ? TimeInForce::Ioc
                        : TimeInForce::Gtc;

            if (rng.Below(100) < static_cast<std::uint64_t>(cfg.pctInvalid)) {
                corrupt(e);
                events.push_back(e);
                continue;  // the book will refuse it, so the pool is unchanged
            }

            sim.AddOrder(e.id, e.side, e.price, e.qty, e.tif, trades);
            const Qty rested = e.qty - settle(e.id);
            if (rested > 0 && e.tif == TimeInForce::Gtc)
                poolAdd(e.id, e.price, rested);

        } else if (roll < static_cast<std::uint64_t>(cfg.pctAdd + cfg.pctCancel)) {
            e.op = Op::Cancel;
            e.id = live[rng.Below(live.size())].id;
            if (rng.Below(100) < static_cast<std::uint64_t>(cfg.pctInvalid)) {
                e.id = kReservedOrderId;  // must be refused, pool unchanged
                events.push_back(e);
                continue;
            }
            sim.CancelOrder(e.id);
            poolRemove(e.id);

        } else {
            const LiveOrder target = live[rng.Below(live.size())];
            e.op = Op::Modify;
            e.id = target.id;

            if (rng.Below(100) < static_cast<std::uint64_t>(cfg.pctResize) &&
                target.qty > 1) {
                e.price = target.price;
                e.qty   = 1 + rng.Below(target.qty - 1);
            } else {
                const std::int64_t off =
                    1 + static_cast<std::int64_t>(rng.Below(cfg.depthTicks));
                const bool buy = target.price <= toPrice(midIdx);
                e.price = toPrice(clampIdx(buy ? midIdx - off : midIdx + off));
                e.qty   = randQty();
            }

            if (rng.Below(100) < static_cast<std::uint64_t>(cfg.pctInvalid)) {
                corrupt(e);
                events.push_back(e);
                continue;  // refused, so the order stays as it was
            }

            sim.ModifyOrder(e.id, e.price, e.qty, trades);
            const Qty rested = e.qty - settle(e.id);
            poolRemove(e.id);
            if (rested > 0) poolAdd(e.id, e.price, rested);
        }
        events.push_back(e);
    }
    return events;
}

}  // namespace bench

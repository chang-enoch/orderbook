#pragma once

#include "orderBook.h"
#include "types.h"

#include <cstdint>
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

    // Generation drives a scratch baseline book so it knows exactly which
    // orders are still resting. That costs nothing (it happens entirely ahead
    // of the timed run) and it means cancel/modify events name orders that are
    // really there -- otherwise most of them would degrade into a failed hash
    // lookup and the benchmark would measure lookup misses, not book work.
    BookConfig bookCfg;
    bookCfg.minPrice = cfg.bandLow;
    bookCfg.maxPrice = cfg.bandHigh;
    bookCfg.tick     = cfg.tick;
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
    // Applies fills to the pool: makers shrink and drop out when exhausted.
    // Returns how much of `taker` was filled.
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

    Price   mid = cfg.midStart;
    OrderId nextId = 1;

    auto clamp = [&](Price p) {
        if (p < cfg.bandLow) return cfg.bandLow;
        if (p > cfg.bandHigh) return cfg.bandHigh;
        return p;
    };
    auto randQty = [&] {
        return cfg.qtyMin + rng.Below(cfg.qtyMax - cfg.qtyMin + 1);
    };

    for (std::size_t i = 0; i < cfg.events; ++i) {
        // Random walk the mid roughly every 16 events.
        if (rng.Below(16) == 0) {
            mid = clamp(mid + (rng.Below(2) ? cfg.tick : -cfg.tick));
        }

        const std::uint64_t roll = rng.Below(100);
        Event e{};

        if (roll < static_cast<std::uint64_t>(cfg.pctAdd) || live.empty()) {
            const bool buy = rng.Below(2) == 0;
            const bool aggressive =
                rng.Below(100) < static_cast<std::uint64_t>(cfg.pctAggressive);
            const Price offset =
                static_cast<Price>(1 + rng.Below(cfg.depthTicks)) * cfg.tick;

            e.op   = Op::Add;
            e.id   = nextId++;
            e.side = buy ? Side::Buy : Side::Sell;
            e.qty  = randQty();
            // Passive rests away from the touch; aggressive reaches across it.
            e.price = aggressive ? clamp(buy ? mid + offset : mid - offset)
                                 : clamp(buy ? mid - offset : mid + offset);
            e.tif = (aggressive &&
                     rng.Below(100) < static_cast<std::uint64_t>(cfg.pctIoc))
                        ? TimeInForce::Ioc
                        : TimeInForce::Gtc;

            sim.AddOrder(e.id, e.side, e.price, e.qty, e.tif, trades);
            const Qty rested = e.qty - settle(e.id);
            if (rested > 0 && e.tif == TimeInForce::Gtc)
                poolAdd(e.id, e.price, rested);

        } else if (roll < static_cast<std::uint64_t>(cfg.pctAdd + cfg.pctCancel)) {
            e.op = Op::Cancel;
            e.id = live[rng.Below(live.size())].id;
            sim.CancelOrder(e.id);
            poolRemove(e.id);

        } else {
            const LiveOrder target = live[rng.Below(live.size())];
            e.op = Op::Modify;
            e.id = target.id;

            if (rng.Below(100) < static_cast<std::uint64_t>(cfg.pctResize) &&
                target.qty > 1) {
                // Same price, strictly smaller quantity: the in-place,
                // priority-preserving path.
                e.price = target.price;
                e.qty   = 1 + rng.Below(target.qty - 1);
            } else {
                // Reprice: cancel/replace to the back of a new level, and it
                // may cross.
                const Price offset =
                    static_cast<Price>(1 + rng.Below(cfg.depthTicks)) * cfg.tick;
                const bool buy = target.price <= mid;
                e.price = clamp(buy ? mid - offset : mid + offset);
                e.qty   = randQty();
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

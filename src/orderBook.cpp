#include "orderBook.h"

#include <algorithm>

OrderBook::OrderBook(const BookConfig& cfg) {
    // The baseline does not use the price band; it is accepted so both engines
    // share a constructor signature. Reserving the index is the one cheap thing
    // worth doing -- rehashing mid-run would show up as pure noise in the tail.
    index_.reserve(cfg.maxOrders);
}

Price OrderBook::BestBid() const {
    return bids_.empty() ? kNoBid : bids_.begin()->first;
}

Price OrderBook::BestAsk() const {
    return asks_.empty() ? kNoAsk : asks_.begin()->first;
}

Qty OrderBook::QtyAt(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.qty;
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.qty;
}

void OrderBook::Snapshot(std::vector<std::pair<Price, Qty>>& bids,
                         std::vector<std::pair<Price, Qty>>& asks) const {
    bids.clear();
    asks.clear();
    for (const auto& [price, level] : bids_) bids.emplace_back(price, level.qty);
    for (const auto& [price, level] : asks_) asks.emplace_back(price, level.qty);
}

// Walks the resting book from the touch outwards while it crosses `limit`,
// filling from the front of each level's FIFO queue.
template <class Map>
Qty OrderBook::MatchAgainst(Map& book, OrderId taker, Price limit, Qty qty,
                            bool takerIsBuy, std::vector<Trade>& out) {
    while (qty > 0 && !book.empty()) {
        auto levelIt = book.begin();
        const Price restingPrice = levelIt->first;
        const bool crosses = takerIsBuy ? (restingPrice <= limit)
                                        : (restingPrice >= limit);
        if (!crosses) break;

        PriceLevel& level = levelIt->second;
        auto& orders = level.orders;

        while (qty > 0 && !orders.empty()) {
            Order& maker = orders.front();
            const Qty fill = std::min(qty, maker.qty);

            out.push_back(Trade{taker, maker.id, restingPrice, fill});
            qty -= fill;
            maker.qty -= fill;
            level.qty -= fill;

            if (maker.qty == 0) {
                index_.erase(maker.id);
                orders.pop_front();
            }
        }

        if (orders.empty()) book.erase(levelIt);
    }
    return qty;
}

Qty OrderBook::Match(OrderId taker, Side side, Price price, Qty qty,
                     std::vector<Trade>& out) {
    if (side == Side::Buy) {
        return MatchAgainst(asks_, taker, price, qty, /*takerIsBuy=*/true, out);
    }
    return MatchAgainst(bids_, taker, price, qty, /*takerIsBuy=*/false, out);
}

void OrderBook::Rest(OrderId id, Side side, Price price, Qty qty) {
    auto& orders = (side == Side::Buy) ? bids_[price] : asks_[price];
    orders.qty += qty;
    orders.orders.push_back(Order{id, side, price, qty});
    index_.emplace(id, Locator{side, price, std::prev(orders.orders.end())});
}

std::size_t OrderBook::AddOrder(OrderId id, Side side, Price price, Qty qty,
                                TimeInForce tif, std::vector<Trade>& out) {
    if (qty == 0 || index_.count(id) != 0) return 0;

    const std::size_t before = out.size();
    const Qty remainder = Match(id, side, price, qty, out);
    if (remainder > 0 && tif == TimeInForce::Gtc) {
        Rest(id, side, price, remainder);
    }
    return out.size() - before;
}

bool OrderBook::CancelOrder(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;

    const Locator loc = it->second;
    index_.erase(it);

    if (loc.side == Side::Buy) {
        auto levelIt = bids_.find(loc.price);
        levelIt->second.qty -= loc.order->qty;
        levelIt->second.orders.erase(loc.order);
        if (levelIt->second.orders.empty()) bids_.erase(levelIt);
    } else {
        auto levelIt = asks_.find(loc.price);
        levelIt->second.qty -= loc.order->qty;
        levelIt->second.orders.erase(loc.order);
        if (levelIt->second.orders.empty()) asks_.erase(levelIt);
    }
    return true;
}

bool OrderBook::ModifyOrder(OrderId id, Price newPrice, Qty newQty,
                            std::vector<Trade>& out) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;

    const Locator loc = it->second;
    const Side side = loc.side;
    const Qty oldQty = loc.order->qty;

    if (newQty == 0) return CancelOrder(id);

    // Pure size-down at the same price: keep queue priority, adjust in place.
    if (newPrice == loc.price && newQty < oldQty) {
        const Qty delta = oldQty - newQty;
        loc.order->qty = newQty;
        if (side == Side::Buy) bids_.find(loc.price)->second.qty -= delta;
        else                   asks_.find(loc.price)->second.qty -= delta;
        return true;
    }
    if (newPrice == loc.price && newQty == oldQty) return true;

    // Everything else is a cancel/replace to the back of the new queue.
    CancelOrder(id);
    AddOrder(id, side, newPrice, newQty, TimeInForce::Gtc, out);
    return true;
}

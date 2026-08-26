#pragma once

#include "poolAllocator.h"
#include "types.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

// Baseline limit order book: idiomatic STL, correct and readable.
//
// Price levels live in an ordered std::map per side (bids descending, asks
// ascending) so the touch is always begin(). Each level holds its resting
// orders in a std::list, which gives FIFO priority and O(1) erase from the
// middle. An id -> locator index makes cancel/modify O(1)-ish.
//
// Templated on the allocator so the identical code can be measured over the
// general-purpose allocator and over a freelist pool. That is the control that
// separates "laid out for the cache" from "stopped calling malloc" -- see
// poolAllocator.h.
struct Order {
    OrderId id;
    Side    side;
    Price   price;
    Qty     qty;
};

template <template <class> class Alloc>
class BasicOrderBook {
public:
    using OrderList = std::list<Order, Alloc<Order>>;

    struct PriceLevel {
        Qty       qty = 0;  // aggregate resting quantity at this price
        OrderList orders;
    };

    // Throws on an invalid config rather than limping along with a silently
    // coerced one. Both engines throw on exactly the same inputs.
    explicit BasicOrderBook(const BookConfig& cfg = {})
        : minPrice_(cfg.minPrice),
          maxPrice_(cfg.maxPrice),
          tick_(cfg.tick),
          maxOrders_(cfg.maxOrders) {
        if (ValidateConfig(cfg) != Status::Ok)
            throw std::invalid_argument("BookConfig: invalid-config");
        // Rehashing mid-run would show up as pure noise in the tail.
        index_.reserve(cfg.maxOrders);
    }

    // Matches against the opposite side, appending trades to `out`, then rests
    // any remainder (Gtc) or discards it (Ioc). The number of trades is the
    // growth of `out`; the return value says whether the order was admitted at
    // all, which a trade count cannot express.
    Status AddOrder(OrderId id, Side side, Price price, Qty qty, TimeInForce tif,
                    std::vector<Trade>& out) {
        const Status v = Admissible(id, price, qty);
        if (v != Status::Ok) return v;
        if (index_.count(id) != 0) return Status::DuplicateId;

        const Qty remainder = Match(id, side, price, qty, out);
        if (remainder > 0 && tif == TimeInForce::Gtc) {
            // Capacity gates *resting*, not admission. An aggressive order
            // reduces the book, so a full book is no reason to refuse it the
            // trades it would have made; only the remainder is turned away.
            if (index_.size() >= maxOrders_) return Status::CapacityExhausted;
            Rest(id, side, price, remainder);
        }
        return Status::Ok;
    }

    Status CancelOrder(OrderId id) {
        if (id == kReservedOrderId) return Status::ReservedId;
        auto it = index_.find(id);
        if (it == index_.end()) return Status::UnknownId;
        Erase(it);
        return Status::Ok;
    }

    // A pure quantity decrease at the same price keeps queue priority.
    // Anything else is a cancel/replace to the back of the new level's queue,
    // and may cross.
    //
    // Validation happens before anything is destroyed. Cancelling first and
    // discovering the replacement is inadmissible would delete the order and
    // report success, which is what the earlier version did.
    Status ModifyOrder(OrderId id, Price newPrice, Qty newQty,
                       std::vector<Trade>& out) {
        if (id == kReservedOrderId) return Status::ReservedId;
        auto it = index_.find(id);
        if (it == index_.end()) return Status::UnknownId;

        if (newQty == 0) {
            Erase(it);
            return Status::Ok;
        }
        const Status v = Admissible(id, newPrice, newQty);
        if (v != Status::Ok) return v;

        const Locator loc = it->second;
        const Side    side = loc.side;
        const Qty     oldQty = loc.order->qty;

        if (newPrice == loc.price) {
            if (newQty < oldQty) {
                const Qty delta = oldQty - newQty;
                loc.order->qty = newQty;
                // The two maps differ in their comparator, so they are distinct
                // types and the branch cannot be factored out.
                if (side == Side::Buy) bids_.find(loc.price)->second.qty -= delta;
                else                   asks_.find(loc.price)->second.qty -= delta;
                return Status::Ok;
            }
            if (newQty == oldQty) return Status::Ok;
        }

        Erase(it);
        const Qty remainder = Match(id, side, newPrice, newQty, out);
        if (remainder > 0) {
            if (index_.size() >= maxOrders_) return Status::CapacityExhausted;
            Rest(id, side, newPrice, remainder);
        }
        return Status::Ok;
    }

    Price BestBid() const { return bids_.empty() ? kNoBid : bids_.begin()->first; }
    Price BestAsk() const { return asks_.empty() ? kNoAsk : asks_.begin()->first; }

    Qty QtyAt(Side side, Price price) const {
        if (side == Side::Buy) {
            auto it = bids_.find(price);
            return it == bids_.end() ? 0 : it->second.qty;
        }
        auto it = asks_.find(price);
        return it == asks_.end() ? 0 : it->second.qty;
    }

    std::size_t OrderCount() const { return index_.size(); }

    void Snapshot(std::vector<std::pair<Price, Qty>>& bids,
                  std::vector<std::pair<Price, Qty>>& asks) const {
        bids.clear();
        asks.clear();
        for (const auto& [price, level] : bids_) bids.emplace_back(price, level.qty);
        for (const auto& [price, level] : asks_) asks.emplace_back(price, level.qty);
    }

private:
    using BidMap = std::map<Price, PriceLevel, std::greater<Price>,
                            Alloc<std::pair<const Price, PriceLevel>>>;
    using AskMap = std::map<Price, PriceLevel, std::less<Price>,
                            Alloc<std::pair<const Price, PriceLevel>>>;

    struct Locator {
        Side                          side;
        Price                         price;
        typename OrderList::iterator  order;
    };
    using Index = std::unordered_map<OrderId, Locator, std::hash<OrderId>,
                                     std::equal_to<OrderId>,
                                     Alloc<std::pair<const OrderId, Locator>>>;

    // The admission rules, applied identically by both engines. The baseline
    // has no structural need for a price band -- a std::map would hold any
    // price -- but enforcing the same rules is what makes "identical semantics"
    // a testable claim rather than an assertion.
    Status Admissible(OrderId id, Price price, Qty qty) const {
        if (id == kReservedOrderId) return Status::ReservedId;
        if (qty == 0) return Status::ZeroQty;
        if (price < minPrice_ || price > maxPrice_) return Status::PriceOutOfBand;
        if (tick_ != 1 && (price - minPrice_) % tick_ != 0)
            return Status::PriceNotOnTick;
        return Status::Ok;
    }

    void Erase(typename Index::iterator it) {
        const Locator loc = it->second;
        index_.erase(it);
        if (loc.side == Side::Buy) EraseFrom(bids_, loc);
        else                       EraseFrom(asks_, loc);
    }

    template <class Map>
    void EraseFrom(Map& book, const Locator& loc) {
        auto levelIt = book.find(loc.price);
        levelIt->second.qty -= loc.order->qty;
        levelIt->second.orders.erase(loc.order);
        if (levelIt->second.orders.empty()) book.erase(levelIt);
    }

    template <class Map>
    Qty MatchAgainst(Map& book, OrderId taker, Price limit, Qty qty,
                     bool takerIsBuy, std::vector<Trade>& out) {
        while (qty > 0 && !book.empty()) {
            auto levelIt = book.begin();
            const Price restingPrice = levelIt->first;
            if (takerIsBuy ? (restingPrice > limit) : (restingPrice < limit)) break;

            PriceLevel& level = levelIt->second;
            auto& orders = level.orders;
            while (qty > 0 && !orders.empty()) {
                Order& maker = orders.front();
                const Qty fill = std::min(qty, maker.qty);
                out.push_back(Trade{taker, maker.id, restingPrice, fill});
                qty -= fill;
                maker.qty -= fill;
                level.qty -= fill;
                if (maker.qty != 0) break;
                index_.erase(maker.id);
                orders.pop_front();
            }
            if (orders.empty()) book.erase(levelIt);
        }
        return qty;
    }

    Qty Match(OrderId taker, Side side, Price price, Qty qty,
              std::vector<Trade>& out) {
        return side == Side::Buy
                   ? MatchAgainst(asks_, taker, price, qty, true, out)
                   : MatchAgainst(bids_, taker, price, qty, false, out);
    }

    void Rest(OrderId id, Side side, Price price, Qty qty) {
        auto& level = (side == Side::Buy) ? bids_[price] : asks_[price];
        level.qty += qty;
        level.orders.push_back(Order{id, side, price, qty});
        index_.emplace(id, Locator{side, price, std::prev(level.orders.end())});
    }

    Price       minPrice_;
    Price       maxPrice_;
    Price       tick_;
    std::size_t maxOrders_;

    BidMap bids_;
    AskMap asks_;
    Index  index_;
};

// The two variants under test. Identical code, identical container shapes; the
// only difference is where the nodes come from.
using OrderBook = BasicOrderBook<std::allocator>;

// The two allocator controls. Identical code, identical container shapes; the
// only difference is where the nodes come from and how they are spread out.
// See poolAllocator.h for why one control is not enough.
using PooledOrderBook    = BasicOrderBook<PoolAlloc>;         // no malloc, clustered
using ScatteredOrderBook = BasicOrderBook<ScatterPoolAlloc>;  // no malloc, spread out

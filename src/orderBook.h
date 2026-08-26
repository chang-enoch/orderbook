#pragma once

#include "types.h"

#include <cstddef>
#include <functional>
#include <list>
#include <map>
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
// The cost of this shape is inherent, and is exactly what the optimized book
// exists to remove: a red-black node per price level, a heap allocation per
// resting order, and pointer chasing on every match step.
struct Order {
    OrderId id;
    Side    side;
    Price   price;
    Qty     qty;
};

struct PriceLevel {
    Qty              qty = 0;  // aggregate resting quantity at this price
    std::list<Order> orders;
};

class OrderBook {
public:
    explicit OrderBook(const BookConfig& cfg = {});

    // Matches against the opposite side, appending trades to `out`, then rests
    // any remainder (Gtc) or discards it (Ioc). Returns the number of trades
    // appended. Rejects duplicate ids and zero quantities.
    std::size_t AddOrder(OrderId id, Side side, Price price, Qty qty,
                         TimeInForce tif, std::vector<Trade>& out);

    bool CancelOrder(OrderId id);

    // A pure quantity decrease at the same price keeps queue priority.
    // Anything else (price change, or quantity increase) is a cancel/replace
    // to the back of the new level's queue, and may cross.
    bool ModifyOrder(OrderId id, Price newPrice, Qty newQty,
                     std::vector<Trade>& out);

    Price BestBid() const;
    Price BestAsk() const;

    // Introspection, used by the conformance test to compare engines.
    Qty         QtyAt(Side side, Price price) const;
    std::size_t OrderCount() const { return index_.size(); }
    void        Snapshot(std::vector<std::pair<Price, Qty>>& bids,
                         std::vector<std::pair<Price, Qty>>& asks) const;

private:
    using BidMap = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskMap = std::map<Price, PriceLevel, std::less<Price>>;

    // The order's list iterator stays stable across list edits, so it is kept
    // directly. The level is found via one map lookup on the side's map --
    // storing an iterator here would need a per-side type, since the two maps
    // differ in their comparator and so in their iterator type.
    struct Locator {
        Side                       side;
        Price                      price;
        std::list<Order>::iterator order;
    };

    // Consumes `qty` against the resting side opposite to `side`, appending
    // trades. Returns the unfilled remainder.
    Qty Match(OrderId taker, Side side, Price price, Qty qty,
              std::vector<Trade>& out);

    template <class Map>
    Qty MatchAgainst(Map& book, OrderId taker, Price limit, Qty qty,
                     bool takerIsBuy, std::vector<Trade>& out);

    void Rest(OrderId id, Side side, Price price, Qty qty);

    BidMap                                bids_;
    AskMap                                asks_;
    std::unordered_map<OrderId, Locator>  index_;
};

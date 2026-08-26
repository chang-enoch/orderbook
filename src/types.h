#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

using Price   = std::int64_t;
using Qty     = std::uint64_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

// Gtc: any unfilled remainder rests on the book.
// Ioc: any unfilled remainder is discarded.
enum class TimeInForce : std::uint8_t { Gtc, Ioc };

struct Trade {
    OrderId taker;
    OrderId maker;
    Price   price;
    Qty     qty;

    friend bool operator==(const Trade& a, const Trade& b) {
        return a.taker == b.taker && a.maker == b.maker &&
               a.price == b.price && a.qty == b.qty;
    }
};

// Returned by BestBid()/BestAsk() when that side is empty. Chosen so that the
// usual crossing comparisons (bid >= ask) are false against an empty side.
inline constexpr Price kNoBid = std::numeric_limits<Price>::min();
inline constexpr Price kNoAsk = std::numeric_limits<Price>::max();

// Both engines take the same config so the benchmark can build them uniformly.
// The baseline ignores the price band; the optimized book indexes into a flat
// array over it and rejects orders outside.
struct BookConfig {
    Price       minPrice  = 1;
    Price       maxPrice  = 1'000'000;
    Price       tick      = 1;
    std::size_t maxOrders = 1u << 20;
};

inline constexpr Side Opposite(Side s) {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

using Price   = std::int64_t;
using Qty     = std::uint64_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

// Every book operation reports why it was accepted or refused. Without this,
// "rejected" and "matched nothing" are the same answer, and an engine can
// destroy an order while reporting success -- which is exactly how the two
// implementations here were able to disagree silently.
enum class Status : std::uint8_t {
    Ok,
    DuplicateId,
    UnknownId,
    ReservedId,
    ZeroQty,
    PriceOutOfBand,
    PriceNotOnTick,
    CapacityExhausted,
};

inline const char* StatusName(Status s) {
    switch (s) {
        case Status::Ok:                return "ok";
        case Status::DuplicateId:       return "duplicate-id";
        case Status::UnknownId:         return "unknown-id";
        case Status::ReservedId:        return "reserved-id";
        case Status::ZeroQty:           return "zero-qty";
        case Status::PriceOutOfBand:    return "price-out-of-band";
        case Status::PriceNotOnTick:    return "price-not-on-tick";
        case Status::CapacityExhausted: return "capacity-exhausted";
    }
    return "?";
}

// The optimized book's open-addressed table needs one OrderId value to mean
// "empty slot". Reserving the maximum rather than zero matters: zero is a
// perfectly ordinary id for a feed to send, and using it as the sentinel made
// AddOrder silently reject it and CancelOrder read out of bounds. Both engines
// reject this one id explicitly so the rule is visible and testable.
inline constexpr OrderId kReservedOrderId = std::numeric_limits<OrderId>::max();

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

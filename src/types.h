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
    InvalidConfig,
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
        case Status::InvalidConfig:     return "invalid-config";
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

// Both engines take the same config and enforce it identically. The baseline
// has no structural need for a price band -- a std::map would hold any price --
// but enforcing the same rules is what makes "identical semantics" a testable
// claim rather than an assertion.
struct BookConfig {
    Price       minPrice  = 1;
    Price       maxPrice  = 1'000'000;
    Price       tick      = 1;
    std::size_t maxOrders = 1u << 20;
};

// Admission rules stop at the constructor unless the config itself is checked.
// Before this existed, maxOrders == 0 was capacity-exhausted on one engine and
// accepted on the other, and minPrice > maxPrice constructed fine on one and
// threw std::length_error out of the other's vector sizing -- the same class of
// silent divergence as the per-order rules.
inline Status ValidateConfig(const BookConfig& cfg) {
    if (cfg.tick <= 0) return Status::InvalidConfig;
    if (cfg.minPrice > cfg.maxPrice) return Status::InvalidConfig;
    if (cfg.maxOrders == 0) return Status::InvalidConfig;
    return Status::Ok;
}

inline constexpr Side Opposite(Side s) {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

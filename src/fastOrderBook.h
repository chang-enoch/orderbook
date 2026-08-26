#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Cache-optimized limit order book. Same semantics as OrderBook, four
// structural changes:
//
//   1. Price levels live in one flat array indexed by tick, not a red-black
//      tree. Price -> level is an index computation, not a tree descent.
//   2. Best bid/ask come from an occupancy bitmap with cached hints, so the
//      common case is O(1) and the worst case is a few word loads.
//   3. Orders live in a preallocated slab and are linked intrusively by 32-bit
//      handles. Nothing allocates on the hot path.
//   4. id -> handle is a linear-probe open-addressed table, so a lookup touches
//      one cache line instead of chasing a bucket pointer to a heap node.
//
// The cost is a fixed price band: orders outside [minPrice, maxPrice] are
// rejected. Real venues have price bands for the same reason.
class FastOrderBook {
public:
    explicit FastOrderBook(const BookConfig& cfg = {});

    Status AddOrder(OrderId id, Side side, Price price, Qty qty,
                    TimeInForce tif, std::vector<Trade>& out);
    Status CancelOrder(OrderId id);
    Status ModifyOrder(OrderId id, Price newPrice, Qty newQty,
                       std::vector<Trade>& out);

    Price BestBid() const {
        return bestBid_ == kNoIdx ? kNoBid : IdxToPrice(bestBid_);
    }
    Price BestAsk() const {
        return bestAsk_ == kNoIdx ? kNoAsk : IdxToPrice(bestAsk_);
    }

    Qty         QtyAt(Side side, Price price) const;
    std::size_t OrderCount() const { return liveOrders_; }
    void        Snapshot(std::vector<std::pair<Price, Qty>>& bids,
                         std::vector<std::pair<Price, Qty>>& asks) const;

private:
    using Handle = std::uint32_t;
    using Index  = std::uint32_t;

    static constexpr Handle kNull  = 0xFFFFFFFFu;
    static constexpr Index  kNoIdx = 0xFFFFFFFFu;

    // 32 bytes: two orders per cache line, and the fields touched together
    // during a match (qty, next) sit adjacent.
    struct OrderNode {
        OrderId       id;
        Qty           qty;
        Handle        next;
        Handle        prev;
        Index         level;
        std::uint8_t  side;
        std::uint8_t  pad[3];
    };
    static_assert(sizeof(OrderNode) == 32, "OrderNode should stay cache-friendly");

    // 16 bytes: four levels per cache line, so walking adjacent levels during a
    // sweep touches few lines.
    struct FlatLevel {
        Qty    qty  = 0;
        Handle head = kNull;
        Handle tail = kNull;
    };

    // --- price <-> index -----------------------------------------------------
    Index PriceToIdx(Price p) const {
        const Price off = p - minPrice_;
        return static_cast<Index>(tick_ == 1 ? off : off / tick_);
    }
    Price IdxToPrice(Index i) const {
        return minPrice_ + static_cast<Price>(i) * tick_;
    }
    bool InBand(Price p) const {
        if (p < minPrice_ || p > maxPrice_) return false;
        return tick_ == 1 || (p - minPrice_) % tick_ == 0;
    }

    // --- occupancy bitmap ----------------------------------------------------
    //
    // Three-level hierarchy: one bit per tick in l0, one bit per l0 word in l1,
    // one bit per l1 word in l2. A flat bitmap looks fine until the touch level
    // empties and the next occupied level is far away -- then the scan walks
    // every intervening word, which over a million-tick band is thousands of
    // loads and lands squarely in the tail. The hierarchy turns that same scan
    // into about three loads regardless of the gap.
    struct Bitmap {
        std::vector<std::uint64_t> l0, l1, l2;

        void Init(std::size_t bits);
        void Set(Index i);
        void Clear(Index i);
        bool Test(Index i) const {
            return (l0[i >> 6] & (1ull << (i & 63))) != 0;
        }
        // Lowest set bit at or above `from` / highest at or below; kNoIdx if none.
        Index ScanUp(Index from) const;
        Index ScanDown(Index from) const;
    };

    // --- slab ----------------------------------------------------------------
    Handle Alloc();
    void   Free(Handle h);

    // --- id -> handle map ----------------------------------------------------
    //
    // Keys and handles live in separate arrays rather than one array of
    // {key, handle} structs. Probing only ever reads keys, so splitting them
    // halves the bytes the probe walks and puts twice as many candidate keys on
    // each cache line; the handle is touched once, on the slot that matched.
    //
    // The empty-slot sentinel is the maximum OrderId, not zero. Zero is an
    // ordinary id for a feed to send, and using it as the sentinel meant an
    // empty slot compared equal to it: AddOrder(0, ...) was refused as a
    // duplicate, and CancelOrder(0) then indexed the node slab with kNull and
    // read out of bounds. Both engines now reject the one reserved id.
    //
    // Erase uses backward-shift deletion, not tombstones. Tombstones look
    // harmless at first and then quietly destroy this table: an exchange feed
    // inserts far more orders over a session than the table has slots, so every
    // slot eventually holds a tombstone, no probe ever terminates on an empty
    // slot again, and lookups decay toward a full-table scan. Shifting the
    // following cluster back on erase keeps probe chains genuinely bounded, at
    // the cost of a short bounded loop on erase.
    static constexpr OrderId kEmpty = kReservedOrderId;

    static std::uint64_t Hash(OrderId id) {
        // splitmix64 finalizer: cheap, and it scrambles the sequential ids that
        // an exchange feed actually produces.
        std::uint64_t z = id + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // Returns the probe slot holding `id`, or kNoIdx.
    std::size_t MapFind(OrderId id) const;
    void        MapInsert(OrderId id, Handle h);
    void        MapEraseAt(std::size_t slot);

    // The admission rules, identical to the baseline's.
    Status Admissible(OrderId id, Price price, Qty qty) const {
        if (id == kReservedOrderId) return Status::ReservedId;
        if (qty == 0) return Status::ZeroQty;
        if (price < minPrice_ || price > maxPrice_) return Status::PriceOutOfBand;
        if (tick_ != 1 && (price - minPrice_) % tick_ != 0)
            return Status::PriceNotOnTick;
        return Status::Ok;
    }
    bool AtCapacity() const {
        // The probe loop does not terminate on a full table, and linear probing
        // degrades badly past ~70% load, so half-full is a hard limit.
        return liveOrders_ >= nodes_.size() || liveOrders_ * 2 >= keys_.size();
    }

    // --- matching ------------------------------------------------------------
    Qty  Match(OrderId taker, Side side, Price limit, Qty qty,
               std::vector<Trade>& out);
    void Rest(OrderId id, Side side, Index idx, Qty qty);
    void Unlink(Handle h);

    Price       minPrice_;
    Price       maxPrice_;
    Price       tick_;
    std::size_t levelCount_;

    std::vector<FlatLevel> levels_;
    Bitmap                 bidMap_;
    Bitmap                 askMap_;

    std::vector<OrderNode> nodes_;
    Handle                 freeHead_ = kNull;
    Handle                 nodeHigh_ = 0;   // slab bump pointer

    std::vector<OrderId> keys_;   // kEmpty = free, kTomb = erased
    std::vector<Handle>  vals_;
    std::size_t          mask_ = 0;

    Index       bestBid_    = kNoIdx;
    Index       bestAsk_    = kNoIdx;
    std::size_t liveOrders_ = 0;
};

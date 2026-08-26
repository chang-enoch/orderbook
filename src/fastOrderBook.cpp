#include "fastOrderBook.h"

#include <algorithm>
#include <bit>
#include <cassert>

namespace {
std::size_t NextPow2(std::size_t n) {
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}
}  // namespace

FastOrderBook::FastOrderBook(const BookConfig& cfg)
    : minPrice_(cfg.minPrice),
      maxPrice_(cfg.maxPrice),
      tick_(cfg.tick == 0 ? 1 : cfg.tick),
      levelCount_(static_cast<std::size_t>((cfg.maxPrice - cfg.minPrice) /
                                           (cfg.tick == 0 ? 1 : cfg.tick)) +
                  1) {
    levels_.assign(levelCount_, FlatLevel{});
    bidMap_.Init(levelCount_);
    askMap_.Init(levelCount_);

    // The slab is sized once and never grows; the freelist recycles.
    nodes_.resize(cfg.maxOrders == 0 ? 1 : cfg.maxOrders);

    // Load factor stays at or below 50%, which keeps linear probes short.
    const std::size_t cap = NextPow2(std::max<std::size_t>(cfg.maxOrders, 16) * 2);
    keys_.assign(cap, kEmpty);  // kEmpty is the reserved max id, not zero
    vals_.assign(cap, kNull);
    mask_ = cap - 1;
}

// --- bitmap -----------------------------------------------------------------

namespace {
// Shifting a 64-bit word by 64 is undefined, so both mask helpers special-case
// the boundary rather than relying on it.
inline std::uint64_t MaskFrom(unsigned b) {  // bits >= b
    return b >= 64 ? 0ull : (~0ull << b);
}
inline std::uint64_t MaskTo(unsigned b) {  // bits <= b
    return b >= 63 ? ~0ull : ((1ull << (b + 1)) - 1);
}
inline std::size_t WordsFor(std::size_t bits) { return (bits + 63) / 64; }
}  // namespace

void FastOrderBook::Bitmap::Init(std::size_t bits) {
    l0.assign(WordsFor(bits), 0ull);
    l1.assign(WordsFor(l0.size()), 0ull);
    l2.assign(WordsFor(l1.size()), 0ull);
}

void FastOrderBook::Bitmap::Set(Index i) {
    const std::size_t w0 = i >> 6;
    l0[w0] |= 1ull << (i & 63);
    const std::size_t w1 = w0 >> 6;
    l1[w1] |= 1ull << (w0 & 63);
    l2[w1 >> 6] |= 1ull << (w1 & 63);
}

void FastOrderBook::Bitmap::Clear(Index i) {
    const std::size_t w0 = i >> 6;
    l0[w0] &= ~(1ull << (i & 63));
    if (l0[w0] != 0) return;  // summary bits stay valid

    const std::size_t w1 = w0 >> 6;
    l1[w1] &= ~(1ull << (w0 & 63));
    if (l1[w1] != 0) return;

    l2[w1 >> 6] &= ~(1ull << (w1 & 63));
}

FastOrderBook::Index FastOrderBook::Bitmap::ScanUp(Index from) const {
    const std::size_t w0 = from >> 6;
    if (w0 >= l0.size()) return kNoIdx;

    if (const std::uint64_t w = l0[w0] & MaskFrom(from & 63))
        return static_cast<Index>(w0 * 64 + std::countr_zero(w));

    // Next non-empty l0 word, via the l1 summary.
    const std::size_t w1 = w0 >> 6;
    if (const std::uint64_t s = l1[w1] & MaskFrom((w0 & 63) + 1)) {
        const std::size_t n0 = w1 * 64 + std::countr_zero(s);
        return static_cast<Index>(n0 * 64 + std::countr_zero(l0[n0]));
    }

    // Next non-empty l1 word, via the l2 summary.
    for (std::size_t w2 = w1 >> 6; w2 < l2.size(); ++w2) {
        const std::uint64_t s2 =
            (w2 == (w1 >> 6)) ? (l2[w2] & MaskFrom((w1 & 63) + 1)) : l2[w2];
        if (s2 == 0) continue;
        const std::size_t n1 = w2 * 64 + std::countr_zero(s2);
        const std::size_t n0 = n1 * 64 + std::countr_zero(l1[n1]);
        return static_cast<Index>(n0 * 64 + std::countr_zero(l0[n0]));
    }
    return kNoIdx;
}

FastOrderBook::Index FastOrderBook::Bitmap::ScanDown(Index from) const {
    std::size_t w0 = from >> 6;
    if (w0 >= l0.size()) return kNoIdx;

    if (const std::uint64_t w = l0[w0] & MaskTo(from & 63))
        return static_cast<Index>(w0 * 64 + (63 - std::countl_zero(w)));

    const std::size_t w1 = w0 >> 6;
    if ((w0 & 63) != 0) {
        if (const std::uint64_t s = l1[w1] & MaskTo((w0 & 63) - 1)) {
            const std::size_t n0 = w1 * 64 + (63 - std::countl_zero(s));
            return static_cast<Index>(n0 * 64 + (63 - std::countl_zero(l0[n0])));
        }
    }

    const std::size_t top = w1 >> 6;
    for (std::size_t w2 = top + 1; w2-- > 0;) {
        std::uint64_t s2 = l2[w2];
        if (w2 == top) {
            if ((w1 & 63) == 0) continue;
            s2 &= MaskTo((w1 & 63) - 1);
        }
        if (s2 == 0) continue;
        const std::size_t n1 = w2 * 64 + (63 - std::countl_zero(s2));
        const std::size_t n0 = n1 * 64 + (63 - std::countl_zero(l1[n1]));
        return static_cast<Index>(n0 * 64 + (63 - std::countl_zero(l0[n0])));
    }
    return kNoIdx;
}

// --- slab -------------------------------------------------------------------

FastOrderBook::Handle FastOrderBook::Alloc() {
    if (freeHead_ != kNull) {
        const Handle h = freeHead_;
        freeHead_ = nodes_[h].next;
        return h;
    }
    if (nodeHigh_ < nodes_.size()) return nodeHigh_++;
    return kNull;  // slab exhausted; the caller rejects the order
}

void FastOrderBook::Free(Handle h) {
    nodes_[h].next = freeHead_;
    freeHead_ = h;
}

// --- id -> handle map -------------------------------------------------------

std::size_t FastOrderBook::MapFind(OrderId id) const {
    std::size_t i = Hash(id) & mask_;
    while (true) {
        const OrderId k = keys_[i];
        if (k == id) return i;
        if (k == kEmpty) return kNoIdx;
        i = (i + 1) & mask_;
    }
}

void FastOrderBook::MapInsert(OrderId id, Handle h) {
    std::size_t i = Hash(id) & mask_;
    while (keys_[i] != kEmpty) i = (i + 1) & mask_;
    keys_[i] = id;
    vals_[i] = h;
}

// Knuth 6.4 algorithm R. Walks forward from the erased slot, pulling back any
// entry whose ideal slot is at or before the hole, so the cluster stays a
// contiguous probe chain with no tombstone in it.
void FastOrderBook::MapEraseAt(std::size_t i) {
    std::size_t j = i;
    while (true) {
        keys_[i] = kEmpty;
        vals_[i] = kNull;
        while (true) {
            j = (j + 1) & mask_;
            if (keys_[j] == kEmpty) return;
            const std::size_t k = Hash(keys_[j]) & mask_;
            // Does k lie cyclically in (i, j]? If so, entry j is still
            // reachable where it is and must not move.
            const bool mustStay =
                (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (!mustStay) break;
        }
        keys_[i] = keys_[j];
        vals_[i] = vals_[j];
        i = j;
    }
}

// --- book -------------------------------------------------------------------

Qty FastOrderBook::QtyAt(Side side, Price price) const {
    if (!InBand(price)) return 0;
    const Index idx = PriceToIdx(price);
    const Bitmap& bm = (side == Side::Buy) ? bidMap_ : askMap_;
    return bm.Test(idx) ? levels_[idx].qty : 0;
}

void FastOrderBook::Snapshot(std::vector<std::pair<Price, Qty>>& bids,
                             std::vector<std::pair<Price, Qty>>& asks) const {
    bids.clear();
    asks.clear();
    for (Index i = (bestBid_ == kNoIdx ? kNoIdx : bidMap_.ScanDown(bestBid_));
         i != kNoIdx; i = (i == 0 ? kNoIdx : bidMap_.ScanDown(i - 1)))
        bids.emplace_back(IdxToPrice(i), levels_[i].qty);
    for (Index i = (bestAsk_ == kNoIdx ? kNoIdx : askMap_.ScanUp(bestAsk_));
         i != kNoIdx; i = askMap_.ScanUp(i + 1))
        asks.emplace_back(IdxToPrice(i), levels_[i].qty);
}

void FastOrderBook::Rest(OrderId id, Side side, Index idx, Qty qty) {
    const Handle h = Alloc();
    // Callers check AtCapacity() before committing, so this cannot fire; it is
    // kept so a future caller that forgets cannot corrupt the slab silently.
    if (h == kNull) return;

    OrderNode& n = nodes_[h];
    n.id = id;
    n.qty = qty;
    n.next = kNull;
    n.level = idx;
    n.side = static_cast<std::uint8_t>(side);

    FlatLevel& lvl = levels_[idx];
    const bool wasEmpty = (lvl.head == kNull);

    n.prev = lvl.tail;
    if (lvl.tail != kNull) nodes_[lvl.tail].next = h;
    else                   lvl.head = h;
    lvl.tail = h;
    lvl.qty += qty;

    if (side == Side::Buy) {
        if (wasEmpty) bidMap_.Set(idx);
        if (bestBid_ == kNoIdx || idx > bestBid_) bestBid_ = idx;
    } else {
        if (wasEmpty) askMap_.Set(idx);
        if (bestAsk_ == kNoIdx || idx < bestAsk_) bestAsk_ = idx;
    }

    MapInsert(id, h);
    ++liveOrders_;
}

// Removes one node from its level, fixing up occupancy and the best-price hint
// if that emptied the level.
void FastOrderBook::Unlink(Handle h) {
    OrderNode& n = nodes_[h];
    FlatLevel& lvl = levels_[n.level];

    if (n.prev != kNull) nodes_[n.prev].next = n.next;
    else                 lvl.head = n.next;
    if (n.next != kNull) nodes_[n.next].prev = n.prev;
    else                 lvl.tail = n.prev;

    lvl.qty -= n.qty;

    if (lvl.head == kNull) {
        const Index idx = n.level;
        if (n.side == static_cast<std::uint8_t>(Side::Buy)) {
            bidMap_.Clear(idx);
            // Only rescan when the touch itself emptied; otherwise the hint is
            // still correct and this costs nothing.
            if (bestBid_ == idx)
                bestBid_ = (idx == 0) ? kNoIdx : bidMap_.ScanDown(idx - 1);
        } else {
            askMap_.Clear(idx);
            if (bestAsk_ == idx) bestAsk_ = askMap_.ScanUp(idx + 1);
        }
    }
}

Qty FastOrderBook::Match(OrderId taker, Side side, Price limit, Qty qty,
                         std::vector<Trade>& out) {
    const bool takerIsBuy = (side == Side::Buy);

    while (qty > 0) {
        const Index idx = takerIsBuy ? bestAsk_ : bestBid_;
        if (idx == kNoIdx) break;

        const Price restingPrice = IdxToPrice(idx);
        if (takerIsBuy ? (restingPrice > limit) : (restingPrice < limit)) break;

        FlatLevel& lvl = levels_[idx];
        Handle h = lvl.head;
        while (qty > 0 && h != kNull) {
            OrderNode& maker = nodes_[h];
            const Qty fill = std::min(qty, maker.qty);

            out.push_back(Trade{taker, maker.id, restingPrice, fill});
            qty -= fill;
            maker.qty -= fill;
            lvl.qty -= fill;

            if (maker.qty != 0) break;  // taker exhausted against this maker

            const Handle next = maker.next;
            const std::size_t slot = MapFind(maker.id);
            // A resting order is always in the table; if it were not, erasing
            // at kNoIdx would index the key array out of bounds.
            assert(slot != kNoIdx);
            MapEraseAt(slot);
            Free(h);
            --liveOrders_;
            h = next;
        }

        lvl.head = h;
        if (h != kNull) {
            nodes_[h].prev = kNull;
        } else {
            lvl.tail = kNull;
            if (takerIsBuy) {
                askMap_.Clear(idx);
                bestAsk_ = askMap_.ScanUp(idx + 1);
            } else {
                bidMap_.Clear(idx);
                bestBid_ = (idx == 0) ? kNoIdx : bidMap_.ScanDown(idx - 1);
            }
        }
    }
    return qty;
}

Status FastOrderBook::AddOrder(OrderId id, Side side, Price price, Qty qty,
                               TimeInForce tif, std::vector<Trade>& out) {
    const Status v = Admissible(id, price, qty);
    if (v != Status::Ok) return v;
    if (MapFind(id) != kNoIdx) return Status::DuplicateId;

    const Qty remainder = Match(id, side, price, qty, out);
    if (remainder > 0 && tif == TimeInForce::Gtc) {
        // Capacity gates *resting*, not admission. An aggressive order reduces
        // the book, so a full book is no reason to refuse it the trades it
        // would have made; only the remainder is turned away.
        if (AtCapacity()) return Status::CapacityExhausted;
        Rest(id, side, PriceToIdx(price), remainder);
    }
    return Status::Ok;
}

Status FastOrderBook::CancelOrder(OrderId id) {
    if (id == kReservedOrderId) return Status::ReservedId;
    const std::size_t slot = MapFind(id);
    if (slot == kNoIdx) return Status::UnknownId;

    const Handle h = vals_[slot];
    assert(h < nodes_.size());
    Unlink(h);
    Free(h);
    MapEraseAt(slot);      // the slot is already located; no second probe
    --liveOrders_;
    return Status::Ok;
}

Status FastOrderBook::ModifyOrder(OrderId id, Price newPrice, Qty newQty,
                                  std::vector<Trade>& out) {
    if (id == kReservedOrderId) return Status::ReservedId;
    const std::size_t slot = MapFind(id);
    if (slot == kNoIdx) return Status::UnknownId;

    const Handle h = vals_[slot];
    assert(h < nodes_.size());
    OrderNode& n = nodes_[h];
    const Side  side   = static_cast<Side>(n.side);
    const Qty   oldQty = n.qty;
    const Index oldIdx = n.level;

    if (newQty == 0) {
        CancelOrder(id);
        return Status::Ok;
    }

    // Validate the replacement before destroying the original. Cancelling
    // first and then failing the re-add deleted the order and returned
    // success, which is how a reprice to an out-of-band or off-tick price
    // used to make an order vanish.
    const Status v = Admissible(id, newPrice, newQty);
    if (v != Status::Ok) return v;

    if (PriceToIdx(newPrice) == oldIdx) {
        if (newQty < oldQty) {
            // Size-down in place: keeps queue priority.
            levels_[oldIdx].qty -= (oldQty - newQty);
            n.qty = newQty;
            return Status::Ok;
        }
        if (newQty == oldQty) return Status::Ok;
    }

    CancelOrder(id);
    const Qty remainder = Match(id, side, newPrice, newQty, out);
    if (remainder > 0) {
        if (AtCapacity()) return Status::CapacityExhausted;
        Rest(id, side, PriceToIdx(newPrice), remainder);
    }
    return Status::Ok;
}

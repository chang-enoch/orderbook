#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

// Freelist allocators used to decompose the optimized book's advantage.
//
// The optimized book wins on two counts at once: it never calls the
// general-purpose allocator, and it lays memory out for the cache. Those are
// different claims, and one control is not enough to separate them -- a plain
// freelist recycles nodes into a tight cluster, so it removes malloc AND
// improves locality, which would credit the allocator with part of the layout's
// win. Two modes are provided so the two effects can be bracketed:
//
//   Clustered  slots are handed out in address order, so the live set stays
//              compact. Removes malloc and improves locality.
//   Scattered  slots are handed out in a shuffled order over the same region,
//              so the live set is spread out roughly the way malloc leaves it.
//              Removes malloc and leaves locality alone.
//
// Both modes run the identical allocate/deallocate code -- a pop and a push on
// a vector of free slots -- so they cost the same and differ only in where the
// addresses land. baseline -> Scattered is the allocator's share;
// Scattered -> Clustered is the locality share.
//
// Deliberately stateless. A stateful arena would have to propagate into the
// list nested inside each map node, and std::map does not do uses-allocator
// construction for a mapped type that is not allocator-aware -- the nested list
// would quietly fall back to the global allocator and the control would measure
// nothing. Per-type static storage sidesteps that entirely.
//
// Caveat, stated because it affects how the numbers read: the pool is
// process-wide per type and grows to its high-water mark. That is the right
// model for a venue that preallocates at startup, and the wrong model for a
// library.
namespace pool {

class Storage {
public:
    Storage(std::size_t objSize, std::size_t objAlign, bool scatter)
        : size_(objSize < sizeof(void*) ? sizeof(void*) : objSize),
          align_(objAlign < alignof(void*) ? alignof(void*) : objAlign),
          scatter_(scatter) {}

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    // Chunks come from aligned operator new and must go back through the
    // matching aligned operator delete. Holding them in a unique_ptr<byte[]>
    // would free them with delete[] instead -- an alloc/dealloc mismatch, and
    // undefined behaviour that Darwin's ASan does not flag by default.
    ~Storage() {
        for (void* c : chunks_) ::operator delete(c, std::align_val_t{align_});
    }

    void* Take() {
        if (avail_.empty()) Grow();
        void* p = avail_.back();
        avail_.pop_back();
        return p;
    }

    void Give(void* p) { avail_.push_back(p); }

private:
    static std::uint64_t Next(std::uint64_t& s) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }

    void Grow() {
        chunkBytes_ = chunkBytes_ < (1u << 16) ? (1u << 16) : chunkBytes_ * 2;
        void* raw = ::operator new(chunkBytes_, std::align_val_t{align_});
        chunks_.push_back(raw);

        const std::size_t slots = chunkBytes_ / size_;
        const std::size_t base = avail_.size();
        avail_.reserve(base + slots);
        auto* bytes = static_cast<std::byte*>(raw);
        for (std::size_t i = 0; i < slots; ++i) avail_.push_back(bytes + i * size_);

        if (scatter_) {
            // Fisher-Yates over the slots just added. Fixed seed: the point is
            // a reproducible scatter, not randomness.
            for (std::size_t i = slots; i-- > 1;) {
                const std::size_t j = static_cast<std::size_t>(Next(seed_) % (i + 1));
                std::swap(avail_[base + i], avail_[base + j]);
            }
        }
    }

    std::size_t        size_;
    std::size_t        align_;
    bool               scatter_;
    std::vector<void*> chunks_;
    std::vector<void*> avail_;
    std::size_t        chunkBytes_ = 0;
    std::uint64_t      seed_ = 0x9E3779B97F4A7C15ull;
};

struct Clustered { static constexpr bool kScatter = false; };
struct Scattered { static constexpr bool kScatter = true; };

template <class T, class Mode>
Storage& StorageFor() {
    static Storage s(sizeof(T), alignof(T), Mode::kScatter);
    return s;
}

}  // namespace pool

template <class T, class Mode>
class ModePoolAlloc {
public:
    using value_type = T;

    ModePoolAlloc() = default;
    template <class U>
    ModePoolAlloc(const ModePoolAlloc<U, Mode>&) noexcept {}

    T* allocate(std::size_t n) {
        // Only single nodes are pooled; node-based containers never ask for
        // more, and anything that does (a bucket array, say) falls back to the
        // real allocator.
        if (n != 1) return static_cast<T*>(::operator new(n * sizeof(T)));
        return static_cast<T*>(pool::StorageFor<T, Mode>().Take());
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (n != 1) {
            ::operator delete(p);
            return;
        }
        pool::StorageFor<T, Mode>().Give(p);
    }

    template <class U>
    bool operator==(const ModePoolAlloc<U, Mode>&) const noexcept { return true; }
    template <class U>
    bool operator!=(const ModePoolAlloc<U, Mode>&) const noexcept { return false; }
};

template <class T>
using PoolAlloc = ModePoolAlloc<T, pool::Clustered>;
template <class T>
using ScatterPoolAlloc = ModePoolAlloc<T, pool::Scattered>;

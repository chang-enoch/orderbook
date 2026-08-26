#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

// A freelist allocator for node-based containers, used to separate two things
// the benchmark would otherwise confound.
//
// The optimized book beats the baseline on two counts at once: it lays memory
// out for the cache, AND it never calls the general-purpose allocator. Those
// are different claims. Running the *same* std::map/std::list shapes over this
// allocator isolates them: whatever the pooled baseline recovers is the cost of
// malloc, and whatever remains is the cost of the layout.
//
// Deliberately stateless. A stateful arena would have to propagate into the
// list nested inside each map node, and std::map does not do uses-allocator
// construction for a mapped type that is not allocator-aware -- the nested list
// would quietly fall back to the global allocator and the control would be
// measuring nothing. Per-type static storage sidesteps that entirely.
//
// Caveat, stated because it affects how the numbers should be read: the pool is
// process-wide per type and grows to its high-water mark. That is the right
// model for a venue that preallocates at startup, and the wrong model for a
// library.
namespace pool {

class Storage {
public:
    Storage(std::size_t objSize, std::size_t objAlign)
        : size_(objSize < sizeof(void*) ? sizeof(void*) : objSize),
          align_(objAlign < alignof(void*) ? alignof(void*) : objAlign) {}

    void* Take() {
        if (free_ != nullptr) {
            void* p = free_;
            free_ = *static_cast<void**>(p);
            return p;
        }
        if (remaining_ == 0) Grow();
        void* p = cursor_;
        cursor_ += size_;
        --remaining_;
        return p;
    }

    void Give(void* p) {
        *static_cast<void**>(p) = free_;
        free_ = p;
    }

private:
    void Grow() {
        // Geometric growth, so a long run performs a handful of real
        // allocations rather than one per order.
        chunkBytes_ = chunkBytes_ < (1u << 16) ? (1u << 16) : chunkBytes_ * 2;
        chunks_.push_back(std::unique_ptr<std::byte[]>(
            static_cast<std::byte*>(::operator new(chunkBytes_,
                                                   std::align_val_t{align_}))));
        cursor_ = chunks_.back().get();
        remaining_ = chunkBytes_ / size_;
    }

    std::size_t size_;
    std::size_t align_;
    std::vector<std::unique_ptr<std::byte[]>> chunks_;
    std::byte*  cursor_ = nullptr;
    std::size_t remaining_ = 0;
    void*       free_ = nullptr;
    std::size_t chunkBytes_ = 0;
};

template <class T>
Storage& StorageFor() {
    static Storage s(sizeof(T), alignof(T));
    return s;
}

}  // namespace pool

template <class T>
class PoolAlloc {
public:
    using value_type = T;

    PoolAlloc() = default;
    template <class U>
    PoolAlloc(const PoolAlloc<U>&) noexcept {}

    T* allocate(std::size_t n) {
        // Only the single-node case is pooled; node-based containers never ask
        // for more, and anything that does falls back to the real allocator.
        if (n != 1) return static_cast<T*>(::operator new(n * sizeof(T)));
        return static_cast<T*>(pool::StorageFor<T>().Take());
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (n != 1) {
            ::operator delete(p);
            return;
        }
        pool::StorageFor<T>().Give(p);
    }

    template <class U>
    bool operator==(const PoolAlloc<U>&) const noexcept { return true; }
    template <class U>
    bool operator!=(const PoolAlloc<U>&) const noexcept { return false; }
};

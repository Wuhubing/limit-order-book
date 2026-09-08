#ifndef POOL_HPP
#define POOL_HPP

#include <cstddef>
#include <new>
#include <utility>
#include <vector>

// Address-stable chunked slab pool for the engine's Order and Limit objects.
//
// Motivation (Stage D, direction 1: order/lifetime memory management): the
// engine allocates and frees one Order or Limit per add/cancel/modify/market
// step, so per-object malloc/free churn and scattered heap addresses dominate
// the object-lifecycle cost and hurt cache locality during FIFO list walks.
// This pool replaces that churn with O(1) slot reuse over chunk-adjacent
// objects.
//
// Properties:
//  * Address stability: a live object's address never changes. Slots are carved
//    out of fixed chunks; Orders/Limits are linked by raw pointers throughout
//    the engine, so chunks are never reallocated or compacted.
//  * Chunked growth: the first chunk holds `InitialChunkSize` objects and every
//    subsequent chunk doubles in size (4096, 8192, 16384, ...). Growth happens
//    only on exhaustion and is a bulk allocation, amortising the allocator cost
//    over many objects. Chunk growth is the only remaining engine allocation
//    and is documented, not hidden.
//  * Intrusive free list: a destroyed slot stores the next-free pointer in its
//    own first word, so there is no per-object header allocation. construct()
//    and destroy() are O(1).
//  * Ownership: the engine releases an object exactly once (when it is removed
//    from every map and unlinked from its queue). The pool never calls a
//    destructor during teardown — it only frees the chunks — so teardown is
//    correct only if the engine has already destroyed every live object.
//  * Single-threaded: the engine is single-threaded; no atomics/locks.
template <typename T, std::size_t InitialChunkSize = 4096>
class Pool {
public:
    Pool() = default;
    ~Pool() { freeChunks(); }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // Placement-construct a T in a recycled (or freshly grown) slot.
    template <typename... Args>
    T* construct(Args&&... args)
    {
        if (freeList_ == nullptr) {
            grow();
        }
        T* slot = freeList_;
        freeList_ = nextOf(slot);
        return ::new (static_cast<void*>(slot)) T(std::forward<Args>(args)...);
    }

    // Destroy a live T and push its slot onto the free list. Must be called
    // exactly once per constructed object.
    void destroy(T* p) noexcept
    {
        if (p == nullptr) {
            return;
        }
        p->~T();
        nextOf(p) = freeList_;
        freeList_ = p;
    }

    std::size_t chunkCount() const noexcept { return chunks_.size(); }

private:
    // The first word of a dead slot is reused as the intrusive free-list link.
    static T*& nextOf(T* slot) noexcept
    {
        return *reinterpret_cast<T**>(static_cast<void*>(slot));
    }

    void grow()
    {
        const std::size_t count = nextChunkSize_;
        nextChunkSize_ *= 2;

        void* raw = ::operator new(count * sizeof(T),
                                   static_cast<std::align_val_t>(alignof(T)));
        chunks_.push_back(raw);

        T* base = static_cast<T*>(raw);
        for (std::size_t i = 0; i < count; ++i) {
            T* slot = base + i;
            nextOf(slot) = freeList_;
            freeList_ = slot;
        }
    }

    void freeChunks() noexcept
    {
        for (void* raw : chunks_) {
            ::operator delete(raw, static_cast<std::align_val_t>(alignof(T)));
        }
        chunks_.clear();
        freeList_ = nullptr;
    }

    std::vector<void*> chunks_;
    T* freeList_ = nullptr;
    std::size_t nextChunkSize_ = InitialChunkSize;
};

#endif

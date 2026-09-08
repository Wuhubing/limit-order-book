#include "AllocStats.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

// Bench-only TU: overrides the global allocation operators with plain (non-atomic,
// single-threaded) counters. See AllocStats.hpp for the measurement-honesty note.
//
// Layout: every tracked allocation stores a small header immediately before the
// returned pointer. The header records the malloc base and the requested size so
// the unsized `operator delete(void*)` form can still account freed bytes. The
// header + alignment slack are carved out of the single malloc'd block and are
// NOT charged to the counters (only the requested size n is charged).

namespace {

struct Counters {
    std::uint64_t allocCount = 0;
    std::uint64_t freeCount = 0;
    std::uint64_t allocBytes = 0;
    std::uint64_t freeBytes = 0;
};

Counters g_counters;

struct Header {
    void* base;
    std::size_t size;
};

void* trackedAlloc(std::size_t n, std::size_t align) noexcept
{
    const std::size_t hs = sizeof(Header);
    if (n == 0) n = 1;
    std::size_t total = n + align + hs;
    if (total < n) return nullptr; // overflow guard
    void* base = std::malloc(total);
    if (base == nullptr) return nullptr;
    const std::uintptr_t userAddr =
        (reinterpret_cast<std::uintptr_t>(base) + hs + (align - 1)) &
        ~(static_cast<std::uintptr_t>(align) - 1);
    Header* h = reinterpret_cast<Header*>(userAddr - hs);
    h->base = base;
    h->size = n;
    g_counters.allocCount += 1;
    g_counters.allocBytes += n;
    return reinterpret_cast<void*>(userAddr);
}

void trackedFree(void* p) noexcept
{
    if (p == nullptr) return;
    const std::size_t hs = sizeof(Header);
    Header* h = reinterpret_cast<Header*>(reinterpret_cast<char*>(p) - hs);
    g_counters.freeCount += 1;
    g_counters.freeBytes += h->size;
    std::free(h->base);
}

} // namespace

namespace bench {

AllocSnapshot allocSnapshot()
{
    AllocSnapshot s;
    s.allocCount = g_counters.allocCount;
    s.freeCount = g_counters.freeCount;
    s.allocBytes = g_counters.allocBytes;
    s.freeBytes = g_counters.freeBytes;
    return s;
}

} // namespace bench

// ---- throwing new/delete ----
void* operator new(std::size_t n)
{
    void* p = trackedAlloc(n, alignof(std::max_align_t));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n)
{
    void* p = trackedAlloc(n, alignof(std::max_align_t));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { trackedFree(p); }
void operator delete[](void* p) noexcept { trackedFree(p); }
void operator delete(void* p, std::size_t) noexcept { trackedFree(p); }
void operator delete[](void* p, std::size_t) noexcept { trackedFree(p); }

// ---- nothrow new/delete ----
void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
    return trackedAlloc(n, alignof(std::max_align_t));
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
    return trackedAlloc(n, alignof(std::max_align_t));
}
void operator delete(void* p, const std::nothrow_t&) noexcept { trackedFree(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { trackedFree(p); }

// ---- aligned new/delete (C++17) ----
#if defined(__cpp_aligned_new)
void* operator new(std::size_t n, std::align_val_t a)
{
    void* p = trackedAlloc(n, static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a)
{
    void* p = trackedAlloc(n, static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void operator delete(void* p, std::align_val_t) noexcept { trackedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { trackedFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { trackedFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { trackedFree(p); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept
{
    return trackedAlloc(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept
{
    return trackedAlloc(n, static_cast<std::size_t>(a));
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
    trackedFree(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
    trackedFree(p);
}
#endif

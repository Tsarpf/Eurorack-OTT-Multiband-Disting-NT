#include <new>
#include <cstdlib>
#include <sys/reent.h>
#include "newlib_heap.h"

/* ───── minimal newlib re-entrancy ───── */
static struct _reent _reent_obj;
extern "C" {
    struct _reent*        _impure_ptr        = &_reent_obj;
    struct _reent* const  _global_impure_ptr = &_reent_obj;
}

/* ───── minimal C++ runtime ───── */
void* operator new  (std::size_t s)             { return std::malloc(s); }
void  operator delete(void* p)      noexcept    { std::free(p); }
void* operator new[]  (std::size_t s)           { return std::malloc(s); }
void  operator delete[](void* p)    noexcept    { std::free(p); }

/* “nothrow” variants (used by some compilers even with -fno-exceptions) */
void* operator new  (std::size_t s, const std::nothrow_t&) noexcept { return std::malloc(s); }
void* operator new[](std::size_t s, const std::nothrow_t&) noexcept { return std::malloc(s); }
void  operator delete  (void* p, const std::nothrow_t&) noexcept    { std::free(p); }
void  operator delete[](void* p, const std::nothrow_t&) noexcept    { std::free(p); }

#include <cstddef>
#include <cstdint>
#include <cstring>

/* ───── very small bump-allocator ───── */
static PlugHeap* currentHeap = nullptr;

extern "C" void plugHeapInit(PlugHeap* heap, void* base, size_t size)
{
    heap->base = static_cast<uint8_t*>(base);
    heap->size = size;
    heap->brk  = 0;
}

extern "C" void plugHeapUse(PlugHeap* heap)
{
    currentHeap = heap;
}

extern "C" void plugHeapInit(void* base, size_t size)
{
    plugHeapBase = static_cast<uint8_t*>(base);
    plugHeapSize = size;
    plugBrk = 0;
}

extern "C" {

void* malloc(size_t n)
{
    /* 4-byte alignment */
    n = (n + 3u) & ~3u;
    if (!currentHeap || currentHeap->brk + n > currentHeap->size)
        return nullptr;       // out of memory → caller must handle
    void* p = currentHeap->base + currentHeap->brk;
    currentHeap->brk += n;
    return p;
}

void free(void*)                       { /* no-op */ }

void* calloc(size_t nmemb, size_t size)
{
    size_t n = nmemb * size;
    void*  p = malloc(n);
    if (p) std::memset(p, 0, n);
    return p;
}

void* realloc(void* /*old*/, size_t /*n*/) { return nullptr; } // not needed

} // extern "C"

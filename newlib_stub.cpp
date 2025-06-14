#include <new>
#include <cstdlib>
#include <sys/reent.h>

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
/* 8 kB static heap – adjust if needed */
static uint8_t  plugHeap[8192];
static size_t   plugBrk = 0;

extern "C" {

void* malloc(size_t n)
{
    /* 4-byte alignment */
    n = (n + 3u) & ~3u;
    if (plugBrk + n > sizeof(plugHeap))
        return nullptr;       // out of memory → caller must handle
    void* p = &plugHeap[plugBrk];
    plugBrk += n;
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

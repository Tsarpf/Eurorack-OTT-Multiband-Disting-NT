#pragma once
#include <cstddef>
#include <cstdint>

/* Heap size used by newlib_stub allocator */
static constexpr size_t kNewlibHeapSize = 8192;

struct PlugHeap {
    uint8_t* base = nullptr;
    size_t   size = 0;
    size_t   brk  = 0;
};

extern "C" void plugHeapInit(PlugHeap* heap, void* base, size_t size);
extern "C" void plugHeapUse(PlugHeap* heap);


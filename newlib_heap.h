#pragma once
#include <cstddef>
#include <cstdint>

/* Heap size used by newlib_stub allocator */
static constexpr size_t kNewlibHeapSize = 8192;

extern "C" void plugHeapInit(void* base, size_t size);


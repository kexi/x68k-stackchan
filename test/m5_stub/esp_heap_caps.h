#pragma once
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM = 1;
constexpr unsigned MALLOC_CAP_8BIT = 2;
inline bool testHeapFailure = false;
inline void* heap_caps_malloc(std::size_t size, unsigned)
{
    return testHeapFailure ? nullptr : std::malloc(size);
}
inline void heap_caps_free(void* pointer)
{
    std::free(pointer);
}

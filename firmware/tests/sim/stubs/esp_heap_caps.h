// Host stand-in for ESP-IDF's heap_caps (tests/sim): plain malloc.
#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 0
inline void* heap_caps_malloc(size_t size, int) { return std::malloc(size); }
inline void heap_caps_free(void* p) { std::free(p); }

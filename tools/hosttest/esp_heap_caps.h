// host shim so raster.cpp compiles on the Mac for tests/previews
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_SPIRAM 0
static inline void* heap_caps_malloc(size_t n, int) { return malloc(n); }

#pragma once
#include <stdlib.h>
#include <stdint.h>

// Capabilities are ignored on the host: every pool is plain malloc.
#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_DMA      (1 << 3)
#define MALLOC_CAP_INTERNAL (1 << 11)

static inline void* heap_caps_malloc(size_t size, uint32_t) { return malloc(size); }
static inline void  heap_caps_free(void* ptr)               { free(ptr); }

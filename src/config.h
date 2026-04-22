#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#include "macros.h"

#define GC_PAGE_SIZE       KB(64)
#define GC_ALIGNMENT       8
#define GC_MAX_HEAP_SIZE   GB(4)
#define GC_MAX_PAGES       128
#define GC_LARGE_OBJECT_SIZE (GC_PAGE_SIZE / 2)
#define GC_GC_PAGE_WATERMARK 8

#endif

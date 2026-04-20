#ifndef CONFIG_H
#define CONFIG_H

#include "macros.h"

#define GC_PAGE_SIZE       KB(64)
#define GC_GRANULE_SIZE    MB(2)
#define GC_ALIGNMENT       8
#define GC_MAX_HEAP_SIZE   GB(4)

#define GC_NURSERY_PAGES   64
#define GC_OLDGEN_PAGES    512

#endif

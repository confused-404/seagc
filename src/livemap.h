#ifndef LIVEMAP_H
#define LIVEMAP_H

#include "config.h"
#include "types.h"

enum {
  GC_LIVEMAP_BITS_PER_WORD = 64,
  GC_LIVEMAP_SLOT_SIZE = GC_ALIGNMENT,
  GC_LIVEMAP_SLOTS_PER_PAGE = GC_PAGE_SIZE / GC_LIVEMAP_SLOT_SIZE,
  GC_LIVEMAP_WORD_COUNT =
      (GC_LIVEMAP_SLOTS_PER_PAGE + GC_LIVEMAP_BITS_PER_WORD - 1) /
      GC_LIVEMAP_BITS_PER_WORD,
};

typedef struct LiveMap {
  size_t live_bytes;
  size_t live_objects;
  u64 bits[GC_LIVEMAP_WORD_COUNT];
} LiveMap;

/* Initialize an empty page-local liveness map. */
void livemap_init(LiveMap* livemap);
/* Clear all liveness bits and aggregate counters. */
void livemap_reset(LiveMap* livemap);
/* Mark an object once and update aggregate counters. */
bool livemap_mark(LiveMap* livemap, size_t page_offset, size_t object_size);
/* Test whether the object at an offset is marked live. */
bool livemap_is_live(const LiveMap* livemap, size_t page_offset);

#endif

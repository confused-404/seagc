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

void livemap_init(LiveMap* livemap);
void livemap_reset(LiveMap* livemap);
bool livemap_mark(LiveMap* livemap, size_t page_offset, size_t object_size);
bool livemap_is_live(const LiveMap* livemap, size_t page_offset);

#endif

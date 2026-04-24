#include <assert.h>

#include "livemap.h"

void livemap_init(LiveMap* livemap) {
  livemap_reset(livemap); 
}

void livemap_reset(LiveMap *livemap) {
  livemap->live_bytes = 0;
  livemap->live_objects = 0;

  for (size_t i = 0; i < GC_LIVEMAP_WORD_COUNT; i++) {
    livemap->bits[i] = 0;
  }
}

typedef struct LiveMapLocation {
  size_t word_index;
  u64 bit_mask;
} LiveMapLocation;

static LiveMapLocation calculate_livemap_location(size_t page_offset) {
  assert(page_offset < GC_PAGE_SIZE);
  assert(page_offset == ALIGN_UP(page_offset, GC_LIVEMAP_SLOT_SIZE));

  LiveMapLocation lml;

  size_t slot_index = page_offset / GC_LIVEMAP_SLOT_SIZE;
  lml.word_index = slot_index / GC_LIVEMAP_BITS_PER_WORD;
  size_t bit_index = slot_index % GC_LIVEMAP_BITS_PER_WORD;
  lml.bit_mask = ((u64) 1) << bit_index;

  return lml; 
}

bool livemap_is_live(const LiveMap *livemap, size_t page_offset) {
  LiveMapLocation lml = calculate_livemap_location(page_offset);

  return (livemap->bits[lml.word_index] & lml.bit_mask) != 0;
}

bool livemap_mark(LiveMap *livemap, size_t page_offset, size_t object_size) {
  LiveMapLocation lml = calculate_livemap_location(page_offset);

  if ((livemap->bits[lml.word_index] & lml.bit_mask) != 0) {
    return false;
  }

  livemap->bits[lml.word_index] |= lml.bit_mask;
  livemap->live_objects++;
  livemap->live_bytes += object_size;
  return true;
}

#ifndef PAGE_H
#define PAGE_H

#include "types.h"
#include "livemap.h"

typedef enum PageState {
  GC_PAGE_FREE = 0,
  GC_PAGE_ACTIVE,
  GC_PAGE_FULL,
  GC_PAGE_RELOCATING,
  GC_PAGE_LARGE,
} PageState;

typedef enum PageAge {
  GC_PAGE_AGE_YOUNG = 0,
  GC_PAGE_AGE_OLD,
} PageAge;

typedef enum PageSpace {
  GC_SPACE_NURSERY = 0,
  GC_SPACE_SURVIVOR,
  GC_SPACE_OLD,
  GC_SPACE_LARGE,
} PageSpace;

typedef struct PageForwardingEntry {
  size_t old_offset;
  u8* new_payload;
} PageForwardingEntry;

typedef struct Page {
  u8* base;
  u8* top;
  u8* limit;
  size_t used;
  size_t capacity;
  PageState state;
  PageAge age;
  PageSpace space;
  LiveMap livemap;
  PageForwardingEntry* forwarding;
  size_t forwarding_count;
  size_t forwarding_capacity;
} Page;

void page_init(
    Page* page,
    u8* base,
    size_t capacity,
    PageState state,
    PageAge age,
    PageSpace space);
void page_reset(Page* page, PageState state, PageAge age, PageSpace space);
void page_clear_forwarding(Page* page);
void page_promote(Page* page);
void page_release(Page* page);

#endif

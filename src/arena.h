#ifndef ARENA_H
#define ARENA_H

#include "config.h"
#include "types.h"

typedef enum PageState {
  GC_PAGE_FREE = 0,
  GC_PAGE_ACTIVE,
  GC_PAGE_FULL,
  GC_PAGE_LARGE,
} PageState;

typedef struct Page {
  u8* base;
  u8* top;
  u8* limit;
  size_t used;
  size_t capacity;
  PageState state;
} Page;

typedef struct Arena {
  Page pages[GC_MAX_PAGES];
  size_t page_count;
  Page* active_page;
} Arena;

void arena_init(Arena* arena);
void arena_destroy(Arena* arena);
void* arena_alloc(Arena* arena, size_t size);
bool arena_should_collect(const Arena* arena);

#endif

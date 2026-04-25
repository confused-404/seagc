#ifndef ARENA_H
#define ARENA_H

#include "config.h"
#include "types.h"
#include "page.h"
#include "object_header.h"

typedef struct Arena {
  Page pages[GC_MAX_PAGES];
  size_t page_count;
  Page* active_page;
} Arena;

typedef struct AllocLayout {
  size_t header_size;
  size_t total_size;
} AllocLayout;

AllocLayout arena_make_layout(size_t payload_size);
void arena_init(Arena* arena);
void arena_destroy(Arena* arena);
void* arena_alloc(Arena* arena, size_t payload_size);
bool arena_should_collect(const Arena* arena);
Page* arena_find_page(Arena* arena, const void* payload_pointer);
bool arena_mark_object(Arena* arena, const void* payload_pointer);

const ObjectHeader* get_header_pointer(const void* payload_pointer, size_t header_size);

#endif

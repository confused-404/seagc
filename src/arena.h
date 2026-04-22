#ifndef ARENA_H
#define ARENA_H

#include "config.h"
#include "types.h"
#include "page.h"
#include "header.h"

typedef struct Arena {
  Page pages[GC_MAX_PAGES];
  size_t page_count;
  Page* active_page;
} Arena;

typedef struct AllocLayout {
  size_t header_size;
  size_t payload_size;
  size_t total_size;
} AllocLayout;

void arena_init(Arena* arena);
void arena_destroy(Arena* arena);
void* arena_alloc(Arena* arena, Header* header);
void* arena_alloc_large(Arena* arena, Header* header, AllocLayout* alloc_layout);
void* arena_alloc_normal(Arena* arena, Header* header, AllocLayout* alloc_layout);
bool arena_should_collect(const Arena* arena);

Header* get_header_pointer(void* payload_pointer, AllocLayout* alloc_layout);

#endif

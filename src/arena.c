#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "livemap.h"
#include "macros.h"
#include "object_header.h"

static size_t align_size(size_t size) {
  return ALIGN_UP(size, GC_ALIGNMENT);
}

AllocLayout arena_make_layout(size_t payload_size) {
  AllocLayout alloc_layout;

  alloc_layout.header_size = align_size(sizeof(ObjectHeader));
  alloc_layout.total_size = align_size(alloc_layout.header_size + align_size(payload_size));

  if (alloc_layout.total_size == 0) {
    alloc_layout.total_size = GC_ALIGNMENT;
  }

  return alloc_layout;
}

static Page* arena_add_page(Arena* arena, size_t capacity, PageState state) {
  Page* page;

  if (arena->page_count >= GC_MAX_PAGES) {
    return NULL;
  }

  page = &arena->pages[arena->page_count++];
  page->base = calloc(1, capacity);
  if (page->base == NULL) {
    arena->page_count--;
    return NULL;
  }

  page_init(page, page->base, capacity, state);

  return page;
}

static Page* arena_get_active_page(Arena* arena, size_t size) {
  Page* page;

  if (arena->active_page != NULL) {
    size_t remaining = (size_t) (arena->active_page->limit - arena->active_page->top);
    if (remaining >= size) {
      return arena->active_page;
    }
    arena->active_page->state = GC_PAGE_FULL;
    arena->active_page = NULL;
  }

  page = arena_add_page(arena, GC_PAGE_SIZE, GC_PAGE_ACTIVE);
  if (page == NULL) {
    return NULL;
  }

  arena->active_page = page;
  return page;
}

void arena_init(Arena* arena) {
  memset(arena, 0, sizeof(*arena));
}

void arena_destroy(Arena* arena) {
  size_t i;

  for (i = 0; i < arena->page_count; i++) {
    page_release(&arena->pages[i]);
  }

  arena->page_count = 0;
  arena->active_page = NULL;
}

static void* arena_alloc_large(Arena* arena, const ObjectHeader* header, const AllocLayout* alloc_layout) {
  Page* page;
  u8* top;
  ObjectHeader* h_dest;

  page = arena_add_page(arena, alloc_layout->total_size, GC_PAGE_LARGE);
  if (page == NULL) {
    return NULL;
  }

  top = page->top;
  h_dest = (ObjectHeader*) top;
  *h_dest = *header;

  page->top += alloc_layout->total_size;
  page->used = alloc_layout->total_size;
  return (void*) (top + alloc_layout->header_size);
}

static void* arena_alloc_normal(Arena* arena, const ObjectHeader* header, const AllocLayout* alloc_layout) {
  Page* page;
  u8* top;
  ObjectHeader* h_dest;

  page = arena_get_active_page(arena, alloc_layout->total_size);
  if (page == NULL) {
    return NULL;
  }

  top = page->top;
  h_dest = (ObjectHeader*) top;
  *h_dest = *header;

  page->top += alloc_layout->total_size;
  page->used += alloc_layout->total_size;
  return (void*) (top + alloc_layout->header_size);
}

void* arena_alloc(Arena* arena, size_t payload_size) {
  ObjectHeader header;
  AllocLayout alloc_layout;

  header.size = payload_size;
  alloc_layout = arena_make_layout(payload_size);

  if (alloc_layout.total_size > GC_LARGE_OBJECT_SIZE) {
    return arena_alloc_large(arena, &header, &alloc_layout);
  }

  return arena_alloc_normal(arena, &header, &alloc_layout);
}

bool arena_should_collect(const Arena* arena) {
  return (GC_MAX_PAGES - arena->page_count) <= GC_GC_PAGE_WATERMARK;
}

Page* arena_find_page(Arena* arena, const void* payload_pointer) {
  const size_t header_size = ALIGN_UP(sizeof(ObjectHeader), GC_ALIGNMENT);
  const u8* hp = (const u8*) get_header_pointer(payload_pointer, header_size);

  for (size_t i = 0; i < arena->page_count; i++) {
    const u8* ps = arena->pages[i].base;
    const u8* pe = arena->pages[i].limit;

    if (hp >= ps && hp < pe) {
      return &arena->pages[i];
    }
  }

  return NULL;
}

bool arena_mark_object(Arena* arena, const void* payload_pointer) {
  const size_t header_size = ALIGN_UP(sizeof(ObjectHeader), GC_ALIGNMENT);
  Page* page = arena_find_page(arena, payload_pointer);
  if (page == NULL) {
    return false;
  }
  const ObjectHeader* hp = get_header_pointer(payload_pointer, header_size);
  const u8* h_start = (const u8*) hp;

  size_t page_offset = (size_t) (h_start - (const u8*) page->base);

  return livemap_mark(&page->livemap, page_offset, hp->size);
}

const ObjectHeader* get_header_pointer(const void* payload_pointer, size_t header_size) {
  return (const ObjectHeader*) ((const u8*) payload_pointer - header_size);
}

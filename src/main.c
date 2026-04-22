#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "macros.h"
#include "header.h"

static size_t align_size(size_t size) {
  return ALIGN_UP(size, GC_ALIGNMENT);
}

static int arena_page_index(const Arena* arena, const Page* page) {
  size_t i;

  if (page == NULL) {
    return -1;
  }

  for (i = 0; i < arena->page_count; i++) {
    if (&arena->pages[i] == page) {
      return (int) i;
    }
  }

  return -1;
}

static void arena_dump_pages(const Arena* arena) {
  size_t i;

  printf("page_count=%zu active_page=%d\n",
      arena->page_count, arena_page_index(arena, arena->active_page));

  for (i = 0; i < arena->page_count; i++) {
    const Page* page = &arena->pages[i];
    printf("page[%zu] state=%d used=%zu capacity=%zu\n",
        i, (int) page->state, page->used, page->capacity);
  }
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

  page->top = page->base;
  page->limit = page->base + capacity;
  page->used = 0;
  page->capacity = capacity;
  page->state = state;
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
    free(arena->pages[i].base);
    arena->pages[i].base = NULL;
    arena->pages[i].top = NULL;
    arena->pages[i].limit = NULL;
    arena->pages[i].used = 0;
    arena->pages[i].capacity = 0;
    arena->pages[i].state = GC_PAGE_FREE;
  }

  arena->page_count = 0;
  arena->active_page = NULL;
}

void* arena_alloc_large(Arena* arena, Header* header, AllocLayout* alloc_layout) {
  Page* page;

  page = arena_add_page(arena, alloc_layout->total_size, GC_PAGE_LARGE);
  if (page == NULL) {
    return NULL;
  }

  u8* top = page->top;

  Header* h_dest = (Header*) top;
  *h_dest = *header;

  page->top += alloc_layout->total_size;
  page->used = alloc_layout->total_size;
  return (void*) (top + alloc_layout->header_size);
}

void* arena_alloc_normal(Arena* arena, Header* header, AllocLayout* alloc_layout) {
  Page* page;

  page = arena_get_active_page(arena, alloc_layout->total_size);
  if (page == NULL) {
    return NULL;
  }

  u8* top = page->top;

  Header* h_dest = (Header*) top;
  *h_dest = *header;

  page->top += alloc_layout->total_size;
  page->used += alloc_layout->total_size;
  return (void*) (top + alloc_layout->header_size);
}

void* arena_alloc(Arena* arena, Header* header) {
  size_t aligned_meta_size = align_size(sizeof(*header));
  size_t aligned_payload_size = align_size(header->size);
  size_t size = align_size(aligned_meta_size + aligned_payload_size);

  AllocLayout alloc_layout = {
    aligned_meta_size,
    aligned_payload_size,
    size
  };

  if (size == 0) {
    size = GC_ALIGNMENT;
  }

  if (size > GC_LARGE_OBJECT_SIZE) {
    return arena_alloc_large(arena, header, &alloc_layout);
  } else {
    return arena_alloc_normal(arena, header, &alloc_layout);
  }
}

bool arena_should_collect(const Arena* arena) {
  return (GC_MAX_PAGES - arena->page_count) <= GC_GC_PAGE_WATERMARK;
}

int main(void) {
  Arena arena;

  int i;

  arena_init(&arena);

  for (i = 0; i < 1000; i++) {
    void* t;

    Header h = { 1024 };

    t = arena_alloc(&arena, &h);
    assert(t != NULL);

    if (i % 5 == 0) {
      printf("alloc[%d] ptr=%p page_count=%zu active_page=%d\n",
          i, t, arena.page_count, arena_page_index(&arena, arena.active_page));
    }
  }

  arena_dump_pages(&arena);
  printf("pages: %zu\n", arena.page_count);
  printf("active page state: %d\n", arena.active_page != NULL ? (int)arena.active_page->state : -1);
  printf("should collect soon: %s\n", arena_should_collect(&arena) ? "yes" : "no");

  arena_destroy(&arena);
  return 0;
}

#include <assert.h>
#include <stdio.h>

#include "arena.h"

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

int main(void) {
  Arena arena;
  AllocLayout alloc_layout;

  int i;

  arena_init(&arena);

  alloc_layout = arena_make_layout(1024);

  for (i = 0; i < 1000; i++) {
    void* t;
    ObjectHeader* recovered;
    Page* owner;

    t = arena_alloc(&arena, 1024);
    assert(t != NULL);
    recovered = get_header_pointer(t, alloc_layout.header_size);
    assert(recovered->size == 1024);
    owner = arena_find_page(&arena, t, alloc_layout.header_size);
    assert(owner != NULL);
    assert(owner == arena.active_page);

    if (i % 5 == 0) {
      printf("alloc[%d] ptr=%p header=%p stored_size=%zu page_count=%zu active_page=%d owner_page=%d\n",
          i, t, (void*) recovered, recovered->size, arena.page_count,
          arena_page_index(&arena, arena.active_page),
          arena_page_index(&arena, owner));
    }
  }

  arena_dump_pages(&arena);
  printf("pages: %zu\n", arena.page_count);
  printf("active page state: %d\n", arena.active_page != NULL ? (int)arena.active_page->state : -1);
  printf("should collect soon: %s\n", arena_should_collect(&arena) ? "yes" : "no");

  arena_destroy(&arena);
  return 0;
}

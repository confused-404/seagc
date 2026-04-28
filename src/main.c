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

typedef struct ObjectIterationStats {
  size_t objects;
  size_t bytes;
} ObjectIterationStats;

static void count_object(Page* page, const ObjectHeader* header, void* payload, void* user_data) {
  ObjectIterationStats* stats = (ObjectIterationStats*) user_data;
  const size_t header_size = arena_make_layout(0).header_size;
  const ObjectHeader* payload_header = get_header_pointer(payload, header_size);

  assert(page != NULL);
  assert(payload_header == header);
  assert(header->total_size == arena_make_layout(header->size).total_size);

  stats->objects++;
  stats->bytes += header->total_size;
}

static void test_page_livemap_mark(Arena* arena, size_t payload_size) {
  AllocLayout alloc_layout;
  void* payload;
  const ObjectHeader* header;
  Page* owner;
  size_t page_offset;
  bool first_mark_result;
  bool second_mark_result;

  alloc_layout = arena_make_layout(payload_size);

  payload = arena_alloc(arena, payload_size);
  assert(payload != NULL);

  header = get_header_pointer(payload, alloc_layout.header_size);
  assert(header->size == payload_size);
  assert(header->total_size == alloc_layout.total_size);
  assert(header->trace != NULL);
  assert(header->trace->pointer_count == 0);
  assert(header->trace->pointer_offsets == NULL);

  owner = arena_find_page(arena, payload);
  assert(owner != NULL);
  assert(owner == arena->active_page);

  page_offset = (size_t) ((const u8*) header - owner->base);

  first_mark_result = arena_mark_object(arena, payload);
  assert(first_mark_result);
  assert(livemap_is_live(&owner->livemap, page_offset));

  second_mark_result = arena_mark_object(arena, payload);
  assert(!second_mark_result);

  assert(owner->livemap.live_objects == 1);
  assert(owner->livemap.live_bytes == header->total_size);

  printf("livemap_test payload=%p header=%p page_offset=%zu owner_page=%d live_objects=%zu live_bytes=%zu\n",
      payload,
      (const void*) header,
      page_offset,
      arena_page_index(arena, owner),
      owner->livemap.live_objects,
      owner->livemap.live_bytes);

  livemap_reset(&owner->livemap);
}

int main(void) {
  Arena arena;
  const size_t payload_size = 1024;
  ObjectIterationStats stats;

  int i;

  arena_init(&arena);
  test_page_livemap_mark(&arena, payload_size);

  for (i = 0; i < 1000; i++) {
    void* t;

    t = arena_alloc(&arena, payload_size);
    assert(t != NULL);

    if (i % 5 == 0) {
      printf("alloc[%d] ptr=%p page_count=%zu active_page=%d\n",
          i,
          t,
          arena.page_count,
          arena_page_index(&arena, arena.active_page));
    }
  }

  arena_dump_pages(&arena);

  stats.objects = 0;
  stats.bytes = 0;
  arena_for_each_object(&arena, count_object, &stats);
  assert(stats.objects == 1001);
  printf("iterated objects=%zu bytes=%zu\n", stats.objects, stats.bytes);

  printf("pages: %zu\n", arena.page_count);
  printf("active page state: %d\n", arena.active_page != NULL ? (int)arena.active_page->state : -1);
  printf("should collect soon: %s\n", arena_should_collect(&arena) ? "yes" : "no");

  arena_destroy(&arena);
  return 0;
}

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "arena.h"
#include "gc.h"
#include "macros.h"

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

typedef struct Pair {
  GCPtr left;
  GCPtr right;
} Pair;

static const size_t pair_pointer_offsets[] = {
  offsetof(Pair, left),
  offsetof(Pair, right),
};

static const TraceDescriptor pair_trace = {
  .pointer_count = ARRAY_LEN(pair_pointer_offsets),
  .pointer_offsets = pair_pointer_offsets,
};

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

static void test_root_mark(Arena* arena, size_t payload_size) {
  AllocLayout alloc_layout;
  GCPtr rooted;
  const ObjectHeader* header;
  Page* owner;
  size_t page_offset;
  GCRoot root_array[1];
  GCRootSet roots;

  alloc_layout = arena_make_layout(payload_size);

  rooted = arena_alloc(arena, payload_size);
  assert(rooted != NULL);

  header = get_header_pointer(rooted, alloc_layout.header_size);
  owner = arena_find_page(arena, rooted);
  assert(owner != NULL);

  page_offset = (size_t) ((const u8*) header - owner->base);
  assert(!livemap_is_live(&owner->livemap, page_offset));

  root_array[0].slot = &rooted;
  roots.roots = root_array;
  roots.count = ARRAY_LEN(root_array);

  assert(gc_mark_roots(arena, &roots));

  assert(livemap_is_live(&owner->livemap, page_offset));
  assert(owner->livemap.live_objects == 1);
  assert(owner->livemap.live_bytes == header->total_size);

  printf("root_mark_test root=%p owner_page=%d live_objects=%zu live_bytes=%zu\n",
      rooted,
      arena_page_index(arena, owner),
      owner->livemap.live_objects,
      owner->livemap.live_bytes);

  livemap_reset(&owner->livemap);
}

static void test_object_field_mark(Arena* arena, size_t payload_size) {
  Pair* parent;
  GCPtr child;
  Page* parent_page;
  Page* child_page;
  const ObjectHeader* child_header;
  size_t child_page_offset;

  parent = (Pair*) arena_alloc_traced(arena, sizeof(*parent), &pair_trace);
  assert(parent != NULL);

  child = arena_alloc(arena, payload_size);
  assert(child != NULL);

  parent->left = child;
  parent->right = NULL;

  parent_page = arena_find_page(arena, parent);
  child_page = arena_find_page(arena, child);
  assert(parent_page != NULL);
  assert(child_page != NULL);

  child_header = get_header_pointer(child, arena_make_layout(payload_size).header_size);
  child_page_offset = (size_t) ((const u8*) child_header - child_page->base);
  assert(!livemap_is_live(&child_page->livemap, child_page_offset));

  assert(arena_mark_object(arena, parent));
  arena_mark_object_fields(arena, parent);

  assert(livemap_is_live(&child_page->livemap, child_page_offset));

  printf("field_mark_test parent=%p child=%p parent_page=%d child_page=%d\n",
      (void*) parent,
      child,
      arena_page_index(arena, parent_page),
      arena_page_index(arena, child_page));

  livemap_reset(&parent_page->livemap);
  if (child_page != parent_page) {
    livemap_reset(&child_page->livemap);
  }
}

static void test_transitive_root_mark(Arena* arena, size_t payload_size) {
  GCPtr grandparent_root;
  Pair* grandparent;
  Pair* parent;
  GCPtr child;
  Page* grandparent_page;
  Page* parent_page;
  Page* child_page;
  const ObjectHeader* child_header;
  size_t child_page_offset;
  GCRoot root_array[1];
  GCRootSet roots;

  grandparent_root = arena_alloc_traced(arena, sizeof(*grandparent), &pair_trace);
  assert(grandparent_root != NULL);
  grandparent = (Pair*) grandparent_root;

  parent = (Pair*) arena_alloc_traced(arena, sizeof(*parent), &pair_trace);
  assert(parent != NULL);

  child = arena_alloc(arena, payload_size);
  assert(child != NULL);

  grandparent->left = parent;
  grandparent->right = NULL;
  parent->left = child;
  parent->right = NULL;

  grandparent_page = arena_find_page(arena, grandparent);
  parent_page = arena_find_page(arena, parent);
  child_page = arena_find_page(arena, child);
  assert(grandparent_page != NULL);
  assert(parent_page != NULL);
  assert(child_page != NULL);

  child_header = get_header_pointer(child, arena_make_layout(payload_size).header_size);
  child_page_offset = (size_t) ((const u8*) child_header - child_page->base);
  assert(!livemap_is_live(&child_page->livemap, child_page_offset));

  root_array[0].slot = &grandparent_root;
  roots.roots = root_array;
  roots.count = ARRAY_LEN(root_array);

  assert(gc_mark_roots(arena, &roots));

  assert(livemap_is_live(&child_page->livemap, child_page_offset));

  printf("transitive_mark_test grandparent=%p parent=%p child=%p\n",
      (void*) grandparent,
      (void*) parent,
      child);

  livemap_reset(&grandparent_page->livemap);
  if (parent_page != grandparent_page) {
    livemap_reset(&parent_page->livemap);
  }
  if (child_page != grandparent_page && child_page != parent_page) {
    livemap_reset(&child_page->livemap);
  }
}

int main(void) {
  Arena arena;
  const size_t payload_size = 1024;
  ObjectIterationStats stats;

  int i;

  arena_init(&arena);
  test_page_livemap_mark(&arena, payload_size);
  test_root_mark(&arena, payload_size);
  test_object_field_mark(&arena, payload_size);
  test_transitive_root_mark(&arena, payload_size);

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
  assert(stats.objects == 1007);
  printf("iterated objects=%zu bytes=%zu\n", stats.objects, stats.bytes);

  printf("pages: %zu\n", arena.page_count);
  printf("active page state: %d\n", arena.active_page != NULL ? (int)arena.active_page->state : -1);
  printf("should collect soon: %s\n", arena_should_collect(&arena) ? "yes" : "no");

  arena_destroy(&arena);
  return 0;
}

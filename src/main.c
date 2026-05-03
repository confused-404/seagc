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

  assert(gc_mark(arena, &roots));

  assert(livemap_is_live(&owner->livemap, page_offset));
  assert(owner->livemap.live_objects == 1);
  assert(owner->livemap.live_bytes == header->total_size);

  printf("root_mark_test root=%p owner_page=%d live_objects=%zu live_bytes=%zu\n",
      rooted,
      arena_page_index(arena, owner),
      owner->livemap.live_objects,
      owner->livemap.live_bytes);

  gc_clear_marks(arena);
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

  assert(gc_mark(arena, &roots));

  assert(livemap_is_live(&child_page->livemap, child_page_offset));

  printf("transitive_mark_test grandparent=%p parent=%p child=%p\n",
      (void*) grandparent,
      (void*) parent,
      child);

  gc_clear_marks(arena);
}

static void test_gc_forward_if_relocating(void) {
  Arena arena;
  Pair* original;
  Pair* forwarded;
  Pair* forwarded_again;
  Page* source_page;

  arena_init(&arena);

  original = (Pair*) arena_alloc_traced(&arena, sizeof(*original), &pair_trace);
  assert(original != NULL);
  original->left = NULL;
  original->right = NULL;

  source_page = arena_find_page(&arena, original);
  assert(source_page != NULL);

  source_page->state = GC_PAGE_RELOCATING;

  forwarded = (Pair*) gc_forward_if_relocating(&arena, original);
  assert(forwarded != NULL);
  assert(forwarded != original);
  assert(forwarded->left == NULL);
  assert(forwarded->right == NULL);
  assert(arena_find_page(&arena, forwarded) != source_page);

  forwarded_again = (Pair*) gc_forward_if_relocating(&arena, original);
  assert(forwarded_again == forwarded);

  printf("forward_object_test old=%p new=%p page=%d\n",
      (void*) original,
      (void*) forwarded,
      arena_page_index(&arena, arena_find_page(&arena, forwarded)));

  gc_clear_marks(&arena);
  gc_sweep(&arena);
  arena_destroy(&arena);
}

static void test_gc_collect_evacuates_sparse_page(void) {
  Arena arena;
  Pair* original_root;
  GCPtr child;
  Pair* moved_root;
  GCRoot root_array[1];
  GCRootSet roots;
  Page* source_page;
  Page* destination_page;

  arena_init(&arena);

  original_root = (Pair*) arena_alloc_traced(&arena, sizeof(*original_root), &pair_trace);
  assert(original_root != NULL);

  child = arena_alloc(&arena, 1024);
  assert(child != NULL);

  original_root->left = child;
  original_root->right = NULL;

  source_page = arena_find_page(&arena, original_root);
  assert(source_page != NULL);
  assert(source_page == arena_find_page(&arena, child));

  root_array[0].slot = (GCPtr*) &original_root;
  roots.roots = root_array;
  roots.count = ARRAY_LEN(root_array);

  assert(gc_collect(&arena, &roots));

  moved_root = original_root;
  assert(moved_root != NULL);
  assert(moved_root != (Pair*) child);
  assert(moved_root->left != NULL);
  assert(moved_root->left != child);
  assert(moved_root->right == NULL);

  destination_page = arena_find_page(&arena, moved_root);
  assert(destination_page != NULL);
  assert(destination_page != source_page);
  assert(arena_find_page(&arena, moved_root->left) != source_page);
  assert(source_page->state == GC_PAGE_FREE);

  printf("collect_relocate_test old_page=%d new_page=%d root=%p child=%p\n",
      arena_page_index(&arena, source_page),
      arena_page_index(&arena, destination_page),
      (void*) moved_root,
      moved_root->left);

  arena_destroy(&arena);
}

static void test_reuse_free_normal_page(void) {
  Arena arena;
  size_t original_page_count;
  Page* free_page;
  void* payload;

  arena_init(&arena);

  while (arena.page_count < 2) {
    payload = arena_alloc(&arena, 1024);
    assert(payload != NULL);
  }

  original_page_count = arena.page_count;
  assert(original_page_count > 0);

  free_page = &arena.pages[0];
  assert(free_page->state == GC_PAGE_FULL);

  page_reset(free_page, GC_PAGE_FREE);

  while (arena.active_page != free_page) {
    payload = arena_alloc(&arena, 1024);
    assert(payload != NULL);
    assert(arena.page_count == original_page_count);
  }

  printf("reuse_page_test page=%zu page_count=%zu\n",
      (size_t) 0,
      arena.page_count);

  arena_destroy(&arena);
}

static void test_sweep_dead_normal_pages(void) {
  Arena arena;
  enum { ROOT_COUNT = 20 };
  GCPtr roots_storage[ROOT_COUNT];
  GCRoot root_array[ROOT_COUNT];
  GCRootSet roots;
  Page* dead_page;
  size_t page_count_after_alloc;
  void* payload;
  size_t i;

  arena_init(&arena);

  for (i = 0; i < ROOT_COUNT; i++) {
    roots_storage[i] = arena_alloc(&arena, 1024);
    assert(roots_storage[i] != NULL);
  }
  while (arena.page_count < 2) {
    payload = arena_alloc(&arena, 1024);
    assert(payload != NULL);
  }

  page_count_after_alloc = arena.page_count;
  dead_page = &arena.pages[1];
  assert(dead_page->state == GC_PAGE_ACTIVE || dead_page->state == GC_PAGE_FULL);

  for (i = 0; i < ROOT_COUNT; i++) {
    root_array[i].slot = &roots_storage[i];
  }
  roots.roots = root_array;
  roots.count = ARRAY_LEN(root_array);

  assert(gc_collect(&arena, &roots));

  assert(dead_page->state == GC_PAGE_FREE);
  assert(arena.page_count == page_count_after_alloc);

  while (arena.active_page != dead_page) {
    payload = arena_alloc(&arena, 1024);
    assert(payload != NULL);
    assert(arena.page_count == page_count_after_alloc);
  }

  printf("sweep_test dead_page=%d page_count=%zu\n",
      arena_page_index(&arena, dead_page),
      arena.page_count);

  arena_destroy(&arena);
}

static void test_sweep_and_reuse_large_page(void) {
  Arena arena;
  GCRootSet roots;
  void* first_large;
  void* second_large;
  Page* large_page;
  size_t page_count_after_alloc;

  arena_init(&arena);

  first_large = arena_alloc(&arena, GC_LARGE_OBJECT_SIZE + 1024);
  assert(first_large != NULL);

  large_page = arena_find_page(&arena, first_large);
  assert(large_page != NULL);
  assert(large_page->state == GC_PAGE_LARGE);

  page_count_after_alloc = arena.page_count;
  roots.roots = NULL;
  roots.count = 0;

  assert(gc_collect(&arena, &roots));

  assert(large_page->state == GC_PAGE_FREE);
  assert(arena.page_count == page_count_after_alloc);

  second_large = arena_alloc(&arena, GC_LARGE_OBJECT_SIZE + 512);
  assert(second_large != NULL);
  assert(arena_find_page(&arena, second_large) == large_page);
  assert(large_page->state == GC_PAGE_LARGE);
  assert(arena.page_count == page_count_after_alloc);

  printf("large_reuse_test page=%d page_count=%zu capacity=%zu\n",
      arena_page_index(&arena, large_page),
      arena.page_count,
      large_page->capacity);

  arena_destroy(&arena);
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
  test_gc_forward_if_relocating();
  test_gc_collect_evacuates_sparse_page();
  test_reuse_free_normal_page();
  test_sweep_dead_normal_pages();
  test_sweep_and_reuse_large_page();

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
  printf("iterated objects=%zu bytes=%zu\n", stats.objects, stats.bytes);
  assert(stats.objects == 1007);

  printf("pages: %zu\n", arena.page_count);
  printf("active page state: %d\n", arena.active_page != NULL ? (int)arena.active_page->state : -1);
  printf("should collect soon: %s\n", arena_should_collect(&arena) ? "yes" : "no");

  arena_destroy(&arena);
  return 0;
}

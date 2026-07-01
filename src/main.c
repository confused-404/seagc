#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "arena.h"
#include "gc.h"
#include "macros.h"

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

static size_t test_failures;
static size_t test_count;
static size_t failed_test_count;

#define TEST_LOG(...) \
  do { \
  } while (0)

#define EXPECT_TRUE(condition) \
  do { \
    if (!(condition)) { \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      test_failures++; \
    } \
  } while (0)

#define RUN_TEST(name, call) \
  do { \
    const size_t failures_before = test_failures; \
    test_count++; \
    printf("[ RUN  ] %s\n", (name)); \
    call; \
    if (test_failures == failures_before) { \
      printf("[  OK  ] %s\n", (name)); \
    } else { \
      failed_test_count++; \
      printf("[ FAIL ] %s (%zu failure%s)\n", \
          (name), \
          test_failures - failures_before, \
          test_failures - failures_before == 1u ? "" : "s"); \
    } \
  } while (0)

#define RUN_ARENA_TEST(name, call) \
  do { \
    Arena arena; \
    arena_init(&arena); \
    RUN_TEST((name), (call)); \
    arena_destroy(&arena); \
  } while (0)

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
  assert(owner == arena->nursery_active_page);
  assert(owner->age == GC_PAGE_AGE_YOUNG);
  assert(owner->space == GC_SPACE_NURSERY);

  page_offset = (size_t) ((const u8*) header - owner->base);

  first_mark_result = arena_mark_object(arena, payload);
  assert(first_mark_result);
  assert(livemap_is_live(&owner->livemap, page_offset));

  second_mark_result = arena_mark_object(arena, payload);
  assert(!second_mark_result);

  assert(owner->livemap.live_objects == 1);
  assert(owner->livemap.live_bytes == header->total_size);

  TEST_LOG("livemap_test payload=%p header=%p page_offset=%zu owner_page=%d live_objects=%zu live_bytes=%zu\n",
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

  TEST_LOG("root_mark_test root=%p owner_page=%d live_objects=%zu live_bytes=%zu\n",
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

  assert(GC_STORE(arena, parent, left, child));
  assert(GC_STORE(arena, parent, right, NULL));

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

  TEST_LOG("field_mark_test parent=%p child=%p parent_page=%d child_page=%d\n",
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

  assert(GC_STORE(arena, grandparent, left, parent));
  assert(GC_STORE(arena, grandparent, right, NULL));
  assert(GC_STORE(arena, parent, left, child));
  assert(GC_STORE(arena, parent, right, NULL));

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

  TEST_LOG("transitive_mark_test grandparent=%p parent=%p child=%p\n",
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
  assert(GC_STORE(&arena, original, left, NULL));
  assert(GC_STORE(&arena, original, right, NULL));

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

  TEST_LOG("forward_object_test old=%p new=%p page=%d\n",
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

  assert(GC_STORE(&arena, original_root, left, child));
  assert(GC_STORE(&arena, original_root, right, NULL));

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

  TEST_LOG("collect_relocate_test old_page=%d new_page=%d root=%p child=%p\n",
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
  assert(free_page->age == GC_PAGE_AGE_YOUNG);

  page_reset(free_page, GC_PAGE_FREE, GC_PAGE_AGE_YOUNG, GC_SPACE_NURSERY);

  while (arena.nursery_active_page != free_page) {
    payload = arena_alloc(&arena, 1024);
    assert(payload != NULL);
    assert(arena.page_count == original_page_count);
  }

  TEST_LOG("reuse_page_test page=%zu page_count=%zu\n",
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

  while (arena.nursery_active_page != dead_page) {
    payload = arena_alloc(&arena, 1024);
    assert(payload != NULL);
    assert(arena.page_count == page_count_after_alloc);
  }

  TEST_LOG("sweep_test dead_page=%d page_count=%zu\n",
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

  TEST_LOG("large_reuse_test page=%d page_count=%zu capacity=%zu\n",
      arena_page_index(&arena, large_page),
      arena.page_count,
      large_page->capacity);

  arena_destroy(&arena);
}

static void test_promote_surviving_page(void) {
  Arena arena;
  enum { ROOT_COUNT = 20 };
  GCPtr rooted[ROOT_COUNT];
  GCRoot root_array[ROOT_COUNT];
  GCRootSet roots;
  Page* owner_page;
  void* next_payload;
  Page* next_page;
  size_t i;

  arena_init(&arena);

  for (i = 0; i < ROOT_COUNT; i++) {
    rooted[i] = arena_alloc(&arena, 1024);
    assert(rooted[i] != NULL);
  }

  owner_page = arena_find_page(&arena, rooted[0]);
  assert(owner_page != NULL);
  assert(owner_page->age == GC_PAGE_AGE_YOUNG);
  assert(owner_page == arena.nursery_active_page);

  for (i = 0; i < ROOT_COUNT; i++) {
    root_array[i].slot = &rooted[i];
  }
  roots.roots = root_array;
  roots.count = ROOT_COUNT;

  assert(gc_collect(&arena, &roots));

  assert(owner_page->age == GC_PAGE_AGE_OLD);
  assert(owner_page->state == GC_PAGE_FULL);
  assert(arena.nursery_active_page == NULL);
  assert(arena.survivor_active_page == NULL);

  next_payload = arena_alloc(&arena, 1024);
  assert(next_payload != NULL);

  next_page = arena_find_page(&arena, next_payload);
  assert(next_page != NULL);
  assert(next_page != owner_page);
  assert(next_page->age == GC_PAGE_AGE_YOUNG);
  assert(next_page->space == GC_SPACE_NURSERY);

  TEST_LOG("promote_test old_page=%d new_page=%d old_age=%d new_age=%d\n",
      arena_page_index(&arena, owner_page),
      arena_page_index(&arena, next_page),
      (int) owner_page->age,
      (int) next_page->age);

  arena_destroy(&arena);
}

static void test_minor_collect_old_to_young(void) {
  Arena arena;
  Pair* parent;
  GCPtr parent_root;
  GCPtr child;
  GCPtr first_survivor;
  GCPtr promoted_child;
  Page* parent_page;
  Page* child_page;
  Page* first_survivor_page;
  Page* promoted_child_page;
  const ObjectHeader* header;
  const size_t header_size = arena_make_layout(0).header_size;
  GCRoot root_array[1];
  GCRootSet roots;

  arena_init(&arena);

  parent = (Pair*) arena_alloc_traced(&arena, sizeof(*parent), &pair_trace);
  assert(parent != NULL);
  assert(GC_STORE(&arena, parent, left, NULL));
  assert(GC_STORE(&arena, parent, right, NULL));

  parent_root = parent;
  root_array[0].slot = &parent_root;
  roots.roots = root_array;
  roots.count = ARRAY_LEN(root_array);

  assert(gc_collect(&arena, &roots));

  parent = (Pair*) parent_root;
  parent_page = arena_find_page(&arena, parent);
  assert(parent_page != NULL);
  assert(parent_page->age == GC_PAGE_AGE_OLD);

  child = arena_alloc(&arena, 1024);
  assert(child != NULL);
  child_page = arena_find_page(&arena, child);
  assert(child_page != NULL);
  assert(child_page->age == GC_PAGE_AGE_YOUNG);
  assert(child_page->space == GC_SPACE_NURSERY);

  assert(GC_STORE(&arena, parent, left, child));
  assert(gc_remembered_set_count(&arena) == 1);

  assert(gc_collect_young(&arena, &roots));

  first_survivor = parent->left;
  first_survivor_page = arena_find_page(&arena, first_survivor);
  assert(first_survivor != NULL);
  assert(first_survivor != child);
  assert(child_page->state == GC_PAGE_FREE);
  assert(first_survivor_page != NULL);
  assert(first_survivor_page->age == GC_PAGE_AGE_YOUNG);
  assert(first_survivor_page->space == GC_SPACE_SURVIVOR);
  header = get_header_pointer(first_survivor, header_size);
  assert(header->age == 1);
  assert(gc_remembered_set_count(&arena) == 1);

  assert(gc_collect_young(&arena, &roots));

  promoted_child = parent->left;
  promoted_child_page = arena_find_page(&arena, promoted_child);
  assert(promoted_child != NULL);
  assert(promoted_child != first_survivor);
  assert(first_survivor_page->state == GC_PAGE_FREE);
  assert(promoted_child_page != NULL);
  assert(promoted_child_page->age == GC_PAGE_AGE_OLD);
  assert(promoted_child_page->space == GC_SPACE_OLD);
  header = get_header_pointer(promoted_child, header_size);
  assert(header->age == GC_PROMOTION_AGE);
  assert(gc_remembered_set_count(&arena) == 0);

  TEST_LOG("minor_collect_test parent_page=%d child_page=%d promoted_page=%d remembered=%zu\n",
      arena_page_index(&arena, parent_page),
      arena_page_index(&arena, child_page),
      arena_page_index(&arena, promoted_child_page),
      gc_remembered_set_count(&arena));

  arena_destroy(&arena);
}

static void test_minor_promoted_parent_remembers_young_child(void) {
  Arena arena;
  Pair* parent;
  GCPtr parent_root;
  GCPtr child;
  GCPtr promoted_child;
  Page* parent_page;
  Page* child_page;
  GCRoot root_array[1];
  GCRootSet roots;

  arena_init(&arena);

  parent = (Pair*) arena_alloc_traced(&arena, sizeof(*parent), &pair_trace);
  assert(parent != NULL);
  assert(GC_STORE(&arena, parent, left, NULL));
  assert(GC_STORE(&arena, parent, right, NULL));

  parent_root = parent;
  root_array[0].slot = &parent_root;
  roots.roots = root_array;
  roots.count = ARRAY_LEN(root_array);

  assert(gc_collect_young(&arena, &roots));

  parent = (Pair*) parent_root;
  parent_page = arena_find_page(&arena, parent);
  assert(parent_page != NULL);
  assert(parent_page->age == GC_PAGE_AGE_YOUNG);
  assert(parent_page->space == GC_SPACE_SURVIVOR);

  child = arena_alloc(&arena, 1024);
  assert(child != NULL);
  assert(GC_STORE(&arena, parent, left, child));

  assert(gc_collect_young(&arena, &roots));

  parent = (Pair*) parent_root;
  child = parent->left;
  parent_page = arena_find_page(&arena, parent);
  child_page = arena_find_page(&arena, child);
  assert(parent_page != NULL);
  assert(child_page != NULL);
  assert(parent_page->age == GC_PAGE_AGE_OLD);
  assert(child_page->age == GC_PAGE_AGE_YOUNG);
  assert(parent_page->space == GC_SPACE_OLD);
  assert(child_page->space == GC_SPACE_SURVIVOR);
  assert(gc_remembered_set_count(&arena) == 1);

  assert(gc_collect_young(&arena, &roots));

  parent = (Pair*) parent_root;
  promoted_child = parent->left;
  child_page = arena_find_page(&arena, promoted_child);
  assert(promoted_child != NULL);
  assert(child_page != NULL);
  assert(child_page->age == GC_PAGE_AGE_OLD);
  assert(child_page->space == GC_SPACE_OLD);
  assert(gc_remembered_set_count(&arena) == 0);

  TEST_LOG("minor_promotion_barrier_test parent_page=%d child_page=%d remembered=%zu\n",
      arena_page_index(&arena, parent_page),
      arena_page_index(&arena, child_page),
      gc_remembered_set_count(&arena));

  arena_destroy(&arena);
}

static void test_minor_repair_young_to_young_fields(void) {
  Arena arena;
  Pair* parent;
  Pair* child;
  Pair* original_parent;
  GCPtr parent_root;
  Page* parent_source_page;
  Page* child_source_page;
  GCRoot root_array[1];
  GCRootSet roots;

  arena_init(&arena);

  parent = (Pair*) arena_alloc_traced(&arena, sizeof(*parent), &pair_trace);
  assert(parent != NULL);
  original_parent = parent;
  child = (Pair*) arena_alloc_traced(&arena, sizeof(*child), &pair_trace);
  assert(child != NULL);
  assert(GC_STORE(&arena, parent, left, child));
  assert(GC_STORE(&arena, parent, right, NULL));
  assert(GC_STORE(&arena, child, left, NULL));
  assert(GC_STORE(&arena, child, right, NULL));

  parent_source_page = arena_find_page(&arena, parent);
  child_source_page = arena_find_page(&arena, child);
  assert(parent_source_page != NULL);
  assert(child_source_page != NULL);

  parent_root = parent;
  root_array[0].slot = &parent_root;
  roots.roots = root_array;
  roots.count = ARRAY_LEN(root_array);

  assert(gc_collect_young(&arena, &roots));

  parent = (Pair*) parent_root;
  child = (Pair*) parent->left;
  EXPECT_TRUE(parent != NULL);
  EXPECT_TRUE(child != NULL);
  EXPECT_TRUE(parent != original_parent);
  EXPECT_TRUE(parent_source_page->state == GC_PAGE_FREE);
  EXPECT_TRUE(child_source_page->state == GC_PAGE_FREE);
  EXPECT_TRUE(arena_find_page(&arena, parent)->age == GC_PAGE_AGE_YOUNG);
  EXPECT_TRUE(arena_find_page(&arena, child)->age == GC_PAGE_AGE_YOUNG);
  EXPECT_TRUE(child->left == NULL);
  EXPECT_TRUE(child->right == NULL);

  TEST_LOG("minor_young_repair_test parent=%p child=%p remembered=%zu\n",
      (void*) parent,
      (void*) child,
      gc_remembered_set_count(&arena));

  arena_destroy(&arena);
}

static void test_minor_repairs_remembered_slot_only_in_old_object(void) {
  Arena arena;
  Pair* parent;
  Pair* stable_old_child;
  GCPtr parent_root;
  GCPtr stable_root;
  GCPtr young_child;
  GCPtr original_young_child;
  Page* parent_page;
  Page* stable_page;
  GCRoot root_array[2];
  GCRootSet roots;

  arena_init(&arena);

  parent = (Pair*) arena_alloc_traced(&arena, sizeof(*parent), &pair_trace);
  assert(parent != NULL);
  stable_old_child = (Pair*) arena_alloc_traced(&arena, sizeof(*stable_old_child), &pair_trace);
  assert(stable_old_child != NULL);
  assert(GC_STORE(&arena, parent, left, NULL));
  assert(GC_STORE(&arena, parent, right, stable_old_child));
  assert(GC_STORE(&arena, stable_old_child, left, NULL));
  assert(GC_STORE(&arena, stable_old_child, right, NULL));

  parent_root = parent;
  stable_root = stable_old_child;
  root_array[0].slot = &parent_root;
  root_array[1].slot = &stable_root;
  roots.roots = root_array;
  roots.count = ARRAY_LEN(root_array);

  assert(gc_collect(&arena, &roots));
  parent = (Pair*) parent_root;
  stable_old_child = (Pair*) stable_root;
  parent_page = arena_find_page(&arena, parent);
  stable_page = arena_find_page(&arena, stable_old_child);
  assert(parent_page != NULL);
  assert(stable_page != NULL);
  assert(parent_page->age == GC_PAGE_AGE_OLD);
  assert(stable_page->age == GC_PAGE_AGE_OLD);

  young_child = arena_alloc(&arena, 128);
  assert(young_child != NULL);
  original_young_child = young_child;
  assert(GC_STORE(&arena, parent, left, young_child));
  EXPECT_TRUE(parent->right == stable_old_child);
  EXPECT_TRUE(gc_remembered_set_count(&arena) == 1);

  assert(gc_collect_young(&arena, &roots));

  parent = (Pair*) parent_root;
  EXPECT_TRUE(parent_page->state != GC_PAGE_FREE);
  EXPECT_TRUE(stable_page->state != GC_PAGE_FREE);
  EXPECT_TRUE(parent->left != NULL);
  EXPECT_TRUE(parent->left != original_young_child);
  EXPECT_TRUE(parent->right == stable_old_child);
  EXPECT_TRUE(arena_find_page(&arena, parent->left)->age == GC_PAGE_AGE_YOUNG);
  EXPECT_TRUE(gc_remembered_set_count(&arena) == 1);

  TEST_LOG("minor_old_slot_repair_test parent_page=%d stable_page=%d remembered=%zu\n",
      arena_page_index(&arena, parent_page),
      arena_page_index(&arena, stable_page),
      gc_remembered_set_count(&arena));

  arena_destroy(&arena);
}

static void test_write_barrier_failure_rolls_back_slot(void) {
  Arena arena;
  Pair* parent;
  GCPtr child;
  Page* parent_page;
  bool stored;

  arena_init(&arena);

  parent = (Pair*) arena_alloc_traced(&arena, sizeof(*parent), &pair_trace);
  assert(parent != NULL);
  assert(GC_STORE(&arena, parent, left, NULL));
  assert(GC_STORE(&arena, parent, right, NULL));

  parent_page = arena_find_page(&arena, parent);
  assert(parent_page != NULL);
  page_promote(parent_page);

  child = arena_alloc(&arena, 1024);
  assert(child != NULL);
  assert(arena_find_page(&arena, child)->age == GC_PAGE_AGE_YOUNG);

  gc_test_fail_next_remembered_grow();
  stored = GC_STORE(&arena, parent, left, child);

  EXPECT_TRUE(!stored);
  EXPECT_TRUE(parent->left == NULL);
  EXPECT_TRUE(gc_remembered_set_count(&arena) == 0);

  TEST_LOG("barrier_failure_test stored=%d slot=%p remembered=%zu\n",
      (int) stored,
      parent->left,
      gc_remembered_set_count(&arena));

  arena_destroy(&arena);
}

static void test_write_barrier_failure_preserves_existing_slot(void) {
  Arena arena;
  Pair* parent;
  GCPtr old_child;
  GCPtr young_child;
  Page* parent_page;
  bool stored;

  arena_init(&arena);

  parent = (Pair*) arena_alloc_traced(&arena, sizeof(*parent), &pair_trace);
  assert(parent != NULL);

  old_child = arena_alloc(&arena, 1024);
  assert(old_child != NULL);

  assert(GC_STORE(&arena, parent, left, old_child));
  assert(GC_STORE(&arena, parent, right, NULL));

  parent_page = arena_find_page(&arena, parent);
  assert(parent_page != NULL);
  assert(parent_page == arena_find_page(&arena, old_child));
  page_promote(parent_page);

  young_child = arena_alloc(&arena, 1024);
  assert(young_child != NULL);
  assert(arena_find_page(&arena, young_child)->age == GC_PAGE_AGE_YOUNG);

  gc_test_fail_next_remembered_grow();
  stored = GC_STORE(&arena, parent, left, young_child);

  EXPECT_TRUE(!stored);
  EXPECT_TRUE(parent->left == old_child);
  EXPECT_TRUE(gc_remembered_set_count(&arena) == 0);

  TEST_LOG("barrier_preserve_test stored=%d slot=%p old=%p remembered=%zu\n",
      (int) stored,
      parent->left,
      old_child,
      gc_remembered_set_count(&arena));

  arena_destroy(&arena);
}

static void test_remembered_set_verification_detects_missing_barrier(void) {
  Arena arena;
  Pair* parent;
  GCPtr child;
  Page* parent_page;

  arena_init(&arena);

  parent = (Pair*) arena_alloc_traced(&arena, sizeof(*parent), &pair_trace);
  assert(parent != NULL);
  assert(GC_STORE(&arena, parent, left, NULL));
  assert(GC_STORE(&arena, parent, right, NULL));

  parent_page = arena_find_page(&arena, parent);
  assert(parent_page != NULL);
  page_promote(parent_page);

  child = arena_alloc(&arena, 1024);
  assert(child != NULL);
  assert(arena_find_page(&arena, child)->age == GC_PAGE_AGE_YOUNG);

  /* Intentional bypass: this test proves debug verification catches it. */
  parent->left = child;
  EXPECT_TRUE(!gc_verify_remembered_set(&arena));

  assert(GC_STORE(&arena, parent, left, child));
  EXPECT_TRUE(gc_verify_remembered_set(&arena));
  EXPECT_TRUE(gc_remembered_set_count(&arena) == 1);

  TEST_LOG("missing_barrier_verify_test parent=%p child=%p remembered=%zu\n",
      (void*) parent,
      child,
      gc_remembered_set_count(&arena));

  arena_destroy(&arena);
}

static void test_remembered_sets_are_page_local_and_deduplicated(void) {
  Arena arena;
  Pair* first_parent;
  Pair* second_parent;
  GCPtr first_child;
  GCPtr second_child;
  Page* first_page;
  Page* second_page;

  arena_init(&arena);

  first_parent = (Pair*) arena_alloc_traced(&arena, sizeof(*first_parent), &pair_trace);
  assert(first_parent != NULL);
  assert(GC_STORE(&arena, first_parent, left, NULL));
  assert(GC_STORE(&arena, first_parent, right, NULL));
  first_page = arena_find_page(&arena, first_parent);
  assert(first_page != NULL);

  while (arena.nursery_active_page == first_page) {
    void* filler = arena_alloc(&arena, 1024);
    assert(filler != NULL);
  }

  second_parent = (Pair*) arena_alloc_traced(&arena, sizeof(*second_parent), &pair_trace);
  assert(second_parent != NULL);
  assert(GC_STORE(&arena, second_parent, left, NULL));
  assert(GC_STORE(&arena, second_parent, right, NULL));
  second_page = arena_find_page(&arena, second_parent);
  assert(second_page != NULL);
  assert(second_page != first_page);

  page_promote(first_page);
  page_promote(second_page);

  first_child = arena_alloc(&arena, 128);
  second_child = arena_alloc(&arena, 128);
  assert(first_child != NULL);
  assert(second_child != NULL);

  assert(GC_STORE(&arena, first_parent, left, first_child));
  assert(GC_STORE(&arena, first_parent, left, second_child));
  assert(GC_STORE(&arena, second_parent, left, first_child));

  EXPECT_TRUE(first_page->remembered_set.count == 1);
  EXPECT_TRUE(second_page->remembered_set.count == 1);
  EXPECT_TRUE(gc_remembered_set_count(&arena) == 2);
  EXPECT_TRUE(gc_verify_remembered_set(&arena));

  TEST_LOG("remembered_page_local_test first_page=%d second_page=%d remembered=%zu\n",
      arena_page_index(&arena, first_page),
      arena_page_index(&arena, second_page),
      gc_remembered_set_count(&arena));

  arena_destroy(&arena);
}

static void test_remembered_set_prunes_stale_slots(void) {
  Arena arena;
  Pair* parent;
  GCPtr child;
  Page* parent_page;

  arena_init(&arena);

  parent = (Pair*) arena_alloc_traced(&arena, sizeof(*parent), &pair_trace);
  assert(parent != NULL);
  assert(GC_STORE(&arena, parent, left, NULL));
  assert(GC_STORE(&arena, parent, right, NULL));
  parent_page = arena_find_page(&arena, parent);
  assert(parent_page != NULL);
  page_promote(parent_page);

  child = arena_alloc(&arena, 128);
  assert(child != NULL);
  assert(GC_STORE(&arena, parent, left, child));
  EXPECT_TRUE(gc_remembered_set_count(&arena) == 1);

  assert(GC_STORE(&arena, parent, left, NULL));
  EXPECT_TRUE(gc_remembered_set_count(&arena) == 1);
  EXPECT_TRUE(gc_verify_remembered_set(&arena));

  assert(gc_collect_young(&arena, NULL));
  EXPECT_TRUE(gc_remembered_set_count(&arena) == 0);

  TEST_LOG("remembered_prune_test parent_page=%d remembered=%zu\n",
      arena_page_index(&arena, parent_page),
      gc_remembered_set_count(&arena));

  arena_destroy(&arena);
}

static void test_remembered_set_stress_minor_relocation_repair(void) {
  enum {
    PARENT_COUNT = 32,
  };
  Arena arena;
  GCPtr parents[PARENT_COUNT];
  GCPtr original_left[PARENT_COUNT];
  GCRoot root_array[PARENT_COUNT];
  GCRootSet roots;

  arena_init(&arena);

  for (size_t i = 0; i < PARENT_COUNT; i++) {
    Pair* parent = (Pair*) arena_alloc_traced(&arena, sizeof(*parent), &pair_trace);
    assert(parent != NULL);
    assert(GC_STORE(&arena, parent, left, NULL));
    assert(GC_STORE(&arena, parent, right, NULL));
    parents[i] = parent;
    root_array[i].slot = &parents[i];
  }

  roots.roots = root_array;
  roots.count = ARRAY_LEN(root_array);
  assert(gc_collect(&arena, &roots));

  for (size_t i = 0; i < PARENT_COUNT; i++) {
    Pair* parent = (Pair*) parents[i];
    GCPtr left_child = arena_alloc(&arena, 128);
    GCPtr right_child = arena_alloc(&arena, 128);

    assert(parent != NULL);
    assert(left_child != NULL);
    assert(right_child != NULL);
    assert(arena_find_page(&arena, parent)->age == GC_PAGE_AGE_OLD);
    assert(GC_STORE(&arena, parent, left, left_child));
    assert(GC_STORE(&arena, parent, right, right_child));
    original_left[i] = left_child;
  }

  EXPECT_TRUE(gc_remembered_set_count(&arena) == PARENT_COUNT * 2u);
  EXPECT_TRUE(gc_verify_remembered_set(&arena));

  assert(gc_collect_young(&arena, &roots));
  EXPECT_TRUE(gc_remembered_set_count(&arena) == PARENT_COUNT * 2u);
  EXPECT_TRUE(gc_verify_remembered_set(&arena));

  for (size_t i = 0; i < PARENT_COUNT; i++) {
    Pair* parent = (Pair*) parents[i];

    EXPECT_TRUE(parent->left != NULL);
    EXPECT_TRUE(parent->right != NULL);
    EXPECT_TRUE(parent->left != original_left[i]);
    EXPECT_TRUE(arena_find_page(&arena, parent->left)->age == GC_PAGE_AGE_YOUNG);
  }

  assert(gc_collect_young(&arena, &roots));
  EXPECT_TRUE(gc_remembered_set_count(&arena) == 0);
  EXPECT_TRUE(gc_verify_remembered_set(&arena));

  TEST_LOG("remembered_stress_test parents=%d remembered=%zu\n",
      PARENT_COUNT,
      gc_remembered_set_count(&arena));

  arena_destroy(&arena);
}

static void test_full_collect_rebuilds_remembered_sets(void) {
  Arena arena;
  Pair* parent;
  GCPtr parent_root;
  GCPtr child_root;
  GCRoot root_array[2];
  GCRootSet roots;

  arena_init(&arena);

  parent = (Pair*) arena_alloc_traced(&arena, sizeof(*parent), &pair_trace);
  assert(parent != NULL);
  assert(GC_STORE(&arena, parent, left, NULL));
  assert(GC_STORE(&arena, parent, right, NULL));
  parent_root = parent;
  root_array[0].slot = &parent_root;
  roots.roots = root_array;
  roots.count = 1;

  assert(gc_collect(&arena, &roots));
  parent = (Pair*) parent_root;
  EXPECT_TRUE(arena_find_page(&arena, parent)->age == GC_PAGE_AGE_OLD);

  child_root = arena_alloc(&arena, 128);
  assert(child_root != NULL);
  EXPECT_TRUE(arena_find_page(&arena, child_root)->age == GC_PAGE_AGE_YOUNG);
  assert(GC_STORE(&arena, parent, left, child_root));
  EXPECT_TRUE(gc_remembered_set_count(&arena) == 1);

  root_array[0].slot = &parent_root;
  root_array[1].slot = &child_root;
  roots.roots = root_array;
  roots.count = ARRAY_LEN(root_array);

  assert(gc_collect(&arena, &roots));
  parent = (Pair*) parent_root;
  child_root = parent->left;
  EXPECT_TRUE(child_root != NULL);
  EXPECT_TRUE(arena_find_page(&arena, child_root)->age == GC_PAGE_AGE_OLD);
  EXPECT_TRUE(gc_remembered_set_count(&arena) == 0);
  EXPECT_TRUE(gc_verify_remembered_set(&arena));
  EXPECT_TRUE(gc_stats(&arena)->full_collections == 2);
  EXPECT_TRUE(gc_stats(&arena)->last_collection_reason == GC_REASON_EXPLICIT_FULL);

  TEST_LOG("full_remembered_rebuild_test remembered=%zu full=%zu reason=%d\n",
      gc_remembered_set_count(&arena),
      gc_stats(&arena)->full_collections,
      (int) gc_stats(&arena)->last_collection_reason);

  arena_destroy(&arena);
}

static void test_root_registration_failure_and_deduplication(void) {
  Arena arena;
  Arena handle_arena;
  GCHandle handle;
  GCPtr root = NULL;
  bool registered;
  bool duplicate_registered;
  bool unregistered;
  bool second_unregistered;
  bool handle_initialized;

  arena_init(&arena);

  gc_test_fail_next_root_grow();
  registered = gc_root_register(&arena, &root);
  EXPECT_TRUE(!registered);
  EXPECT_TRUE(arena.roots.count == 0);

  registered = gc_root_register(&arena, &root);
  EXPECT_TRUE(registered);
  EXPECT_TRUE(arena.roots.count == 1);

  duplicate_registered = gc_root_register(&arena, &root);
  EXPECT_TRUE(duplicate_registered);
  EXPECT_TRUE(arena.roots.count == 1);

  unregistered = gc_root_unregister(&arena, &root);
  EXPECT_TRUE(unregistered);
  EXPECT_TRUE(arena.roots.count == 0);

  second_unregistered = gc_root_unregister(&arena, &root);
  EXPECT_TRUE(!second_unregistered);
  EXPECT_TRUE(arena.roots.count == 0);

  arena_init(&handle_arena);
  gc_test_fail_next_root_grow();
  handle_initialized = gc_handle_init(&handle_arena, &handle, NULL);
  EXPECT_TRUE(!handle_initialized);
  EXPECT_TRUE(!handle.active);
  EXPECT_TRUE(handle_arena.roots.count == 0);

  TEST_LOG("root_registry_test registered=%d duplicate=%d unregistered=%d handle=%d roots=%zu\n",
      (int) registered,
      (int) duplicate_registered,
      (int) unregistered,
      (int) handle_initialized,
      arena.roots.count);

  arena_destroy(&handle_arena);
  arena_destroy(&arena);
}

static void test_registered_root_survives_full_collect(void) {
  Arena arena;
  Pair* original;
  GCPtr root;
  Page* source_page;

  arena_init(&arena);

  original = (Pair*) arena_alloc_traced(&arena, sizeof(*original), &pair_trace);
  assert(original != NULL);
  assert(GC_STORE(&arena, original, left, NULL));
  assert(GC_STORE(&arena, original, right, NULL));
  root = original;

  source_page = arena_find_page(&arena, root);
  assert(source_page != NULL);
  assert(gc_root_register(&arena, &root));

  assert(gc_collect(&arena, NULL));

  EXPECT_TRUE(root != NULL);
  EXPECT_TRUE(root != original);
  EXPECT_TRUE(source_page->state == GC_PAGE_FREE);
  EXPECT_TRUE(arena_find_page(&arena, root) != NULL);
  EXPECT_TRUE(((Pair*) root)->left == NULL);
  EXPECT_TRUE(((Pair*) root)->right == NULL);

  TEST_LOG("registered_root_collect_test old=%p new=%p source_state=%d roots=%zu\n",
      (void*) original,
      root,
      (int) source_page->state,
      arena.roots.count);

  arena_destroy(&arena);
}

static void test_unregistered_root_allows_collection(void) {
  Arena arena;
  GCPtr root;
  Page* source_page;

  arena_init(&arena);

  root = arena_alloc(&arena, 1024);
  assert(root != NULL);
  source_page = arena_find_page(&arena, root);
  assert(source_page != NULL);

  assert(gc_root_register(&arena, &root));
  assert(gc_root_unregister(&arena, &root));
  assert(gc_collect(&arena, NULL));

  EXPECT_TRUE(source_page->state == GC_PAGE_FREE);
  EXPECT_TRUE(arena_find_page(&arena, root) == NULL);
  EXPECT_TRUE(arena.roots.count == 0);

  TEST_LOG("unregistered_root_collect_test root=%p source_state=%d roots=%zu\n",
      root,
      (int) source_page->state,
      arena.roots.count);

  arena_destroy(&arena);
}

static void test_handle_survives_minor_collect(void) {
  Arena arena;
  GCHandle handle;
  Pair* original;
  Pair* moved;
  Page* source_page;
  const ObjectHeader* header;
  const size_t header_size = arena_make_layout(0).header_size;

  arena_init(&arena);

  original = (Pair*) arena_alloc_traced(&arena, sizeof(*original), &pair_trace);
  assert(original != NULL);
  assert(GC_STORE(&arena, original, left, NULL));
  assert(GC_STORE(&arena, original, right, NULL));
  source_page = arena_find_page(&arena, original);
  assert(source_page != NULL);

  assert(gc_handle_init(&arena, &handle, original));
  assert(gc_collect_young(&arena, NULL));

  moved = (Pair*) gc_handle_get(&handle);
  EXPECT_TRUE(moved != NULL);
  EXPECT_TRUE(moved != original);
  EXPECT_TRUE(source_page->state == GC_PAGE_FREE);
  EXPECT_TRUE(arena_find_page(&arena, moved) != NULL);
  EXPECT_TRUE(moved->left == NULL);
  EXPECT_TRUE(moved->right == NULL);
  header = get_header_pointer(moved, header_size);
  EXPECT_TRUE(header->age == 1);

  EXPECT_TRUE(gc_handle_set(&handle, NULL));
  EXPECT_TRUE(gc_handle_get(&handle) == NULL);
  EXPECT_TRUE(gc_handle_destroy(&handle));
  EXPECT_TRUE(!handle.active);
  EXPECT_TRUE(arena.roots.count == 0);

  TEST_LOG("handle_minor_collect_test old=%p new=%p source_state=%d roots=%zu\n",
      (void*) original,
      (void*) moved,
      (int) source_page->state,
      arena.roots.count);

  arena_destroy(&arena);
}

static void test_oversized_allocation_is_rejected(void) {
  Arena arena;
  void* payload;

  arena_init(&arena);

  payload = arena_alloc(&arena, SIZE_MAX);

  EXPECT_TRUE(payload == NULL);
  EXPECT_TRUE(arena.page_count == 0);

  TEST_LOG("oversized_alloc_test payload=%p page_count=%zu\n",
      payload,
      arena.page_count);

  arena_destroy(&arena);
}

static void test_oversized_allocation_preserves_existing_arena(void) {
  Arena arena;
  void* first_payload;
  void* oversized_payload;
  Page* active_page;
  size_t page_count;
  size_t used;

  arena_init(&arena);

  first_payload = arena_alloc(&arena, 1024);
  assert(first_payload != NULL);
  active_page = arena.nursery_active_page;
  assert(active_page != NULL);
  page_count = arena.page_count;
  used = active_page->used;

  oversized_payload = arena_alloc(&arena, SIZE_MAX);

  EXPECT_TRUE(oversized_payload == NULL);
  EXPECT_TRUE(arena.page_count == page_count);
  EXPECT_TRUE(arena.nursery_active_page == active_page);
  EXPECT_TRUE(active_page->used == used);

  TEST_LOG("oversized_preserve_test payload=%p page_count=%zu used=%zu\n",
      oversized_payload,
      arena.page_count,
      active_page->used);

  arena_destroy(&arena);
}

static void test_invalid_trace_descriptor_is_rejected(void) {
  Arena arena;
  const size_t invalid_offset_array[] = {
    sizeof(Pair) - sizeof(void*) + 1u,
  };
  const size_t valid_offset_array[] = {
    offsetof(Pair, right),
  };
  TraceDescriptor null_offsets_trace = {
    .pointer_count = 1,
    .pointer_offsets = NULL,
  };
  TraceDescriptor out_of_bounds_trace = {
    .pointer_count = ARRAY_LEN(invalid_offset_array),
    .pointer_offsets = invalid_offset_array,
  };
  TraceDescriptor valid_trace = {
    .pointer_count = ARRAY_LEN(valid_offset_array),
    .pointer_offsets = valid_offset_array,
  };
  void* null_offsets_payload;
  void* out_of_bounds_payload;
  void* valid_payload;

  arena_init(&arena);

  null_offsets_payload = arena_alloc_traced(&arena, sizeof(Pair), &null_offsets_trace);
  out_of_bounds_payload = arena_alloc_traced(&arena, sizeof(Pair), &out_of_bounds_trace);

  EXPECT_TRUE(null_offsets_payload == NULL);
  EXPECT_TRUE(out_of_bounds_payload == NULL);
  EXPECT_TRUE(arena.page_count == 0);

  valid_payload = arena_alloc_traced(&arena, sizeof(Pair), &valid_trace);
  EXPECT_TRUE(valid_payload != NULL);
  EXPECT_TRUE(arena.page_count == 1);

  TEST_LOG("invalid_trace_test null_payload=%p bounds_payload=%p valid_payload=%p page_count=%zu\n",
      null_offsets_payload,
      out_of_bounds_payload,
      valid_payload,
      arena.page_count);

  arena_destroy(&arena);
}

static void test_failed_relocation_preserves_destination_page(void) {
  Arena arena;
  Pair* first_root;
  Pair* second_root;
  GCPtr unrelated_root;
  GCRoot root_array[3];
  GCRootSet roots;
  Page* source_page;
  Page* destination_page;
  bool evacuated;

  arena_init(&arena);

  first_root = (Pair*) arena_alloc_traced(&arena, sizeof(*first_root), &pair_trace);
  assert(first_root != NULL);
  assert(GC_STORE(&arena, first_root, left, NULL));
  assert(GC_STORE(&arena, first_root, right, NULL));

  second_root = (Pair*) arena_alloc_traced(&arena, sizeof(*second_root), &pair_trace);
  assert(second_root != NULL);
  assert(GC_STORE(&arena, second_root, left, NULL));
  assert(GC_STORE(&arena, second_root, right, NULL));

  source_page = arena_find_page(&arena, first_root);
  assert(source_page != NULL);
  assert(source_page == arena_find_page(&arena, second_root));

  while (arena.page_count < 2) {
    void* filler = arena_alloc(&arena, 1024);
    assert(filler != NULL);
  }

  unrelated_root = arena_alloc(&arena, 1024);
  assert(unrelated_root != NULL);
  destination_page = arena_find_page(&arena, unrelated_root);
  assert(destination_page != NULL);
  assert(destination_page != source_page);
  assert(destination_page == arena.nursery_active_page);

  root_array[0].slot = (GCPtr*) &first_root;
  root_array[1].slot = (GCPtr*) &second_root;
  root_array[2].slot = &unrelated_root;
  roots.roots = root_array;
  roots.count = ARRAY_LEN(root_array);

  assert(gc_mark(&arena, &roots));

  gc_test_fail_forwarding_after(1);
  evacuated = gc_evacuate_sparse_pages(&arena, &roots);

  EXPECT_TRUE(!evacuated);
  EXPECT_TRUE(destination_page->state == GC_PAGE_ACTIVE);
  EXPECT_TRUE(arena_find_page(&arena, unrelated_root) == destination_page);
  EXPECT_TRUE(gc_stats(&arena)->copied_bytes == 0);
  EXPECT_TRUE(gc_stats(&arena)->promoted_bytes == 0);

  TEST_LOG("relocation_failure_test evacuated=%d destination_state=%d unrelated_page=%d copied=%zu\n",
      (int) evacuated,
      (int) destination_page->state,
      arena_page_index(&arena, arena_find_page(&arena, unrelated_root)),
      gc_stats(&arena)->copied_bytes);

  arena_destroy(&arena);
}

static void test_failed_relocation_releases_new_destination_page(void) {
  Arena arena;
  Pair* first_root;
  Pair* second_root;
  GCRoot root_array[2];
  GCRootSet roots;
  Page* source_page;
  Page* active_page;
  size_t page_count;
  bool evacuated;

  arena_init(&arena);

  first_root = (Pair*) arena_alloc_traced(&arena, sizeof(*first_root), &pair_trace);
  assert(first_root != NULL);
  assert(GC_STORE(&arena, first_root, left, NULL));
  assert(GC_STORE(&arena, first_root, right, NULL));

  second_root = (Pair*) arena_alloc_traced(&arena, sizeof(*second_root), &pair_trace);
  assert(second_root != NULL);
  assert(GC_STORE(&arena, second_root, left, NULL));
  assert(GC_STORE(&arena, second_root, right, NULL));

  source_page = arena_find_page(&arena, first_root);
  assert(source_page != NULL);
  assert(source_page == arena_find_page(&arena, second_root));

  active_page = arena.nursery_active_page;
  page_count = arena.page_count;
  assert(page_count == 1);

  root_array[0].slot = (GCPtr*) &first_root;
  root_array[1].slot = (GCPtr*) &second_root;
  roots.roots = root_array;
  roots.count = ARRAY_LEN(root_array);

  assert(gc_mark(&arena, &roots));

  gc_test_fail_forwarding_after(1);
  evacuated = gc_evacuate_sparse_pages(&arena, &roots);

  EXPECT_TRUE(!evacuated);
  EXPECT_TRUE(arena.page_count == page_count);
  EXPECT_TRUE(arena.nursery_active_page == active_page);
  EXPECT_TRUE(source_page->state == GC_PAGE_ACTIVE);
  EXPECT_TRUE(source_page->forwarding_count == 0);
  EXPECT_TRUE(gc_stats(&arena)->copied_bytes == 0);
  EXPECT_TRUE(gc_stats(&arena)->promoted_bytes == 0);

  TEST_LOG("relocation_new_page_failure_test evacuated=%d page_count=%zu source_state=%d\n",
      (int) evacuated,
      arena.page_count,
      (int) source_page->state);

  arena_destroy(&arena);
}

static void test_failed_multi_page_relocation_rolls_back_all_sources(void) {
  Arena arena;
  Pair* first_root;
  Pair* second_root;
  Page* first_page;
  Page* second_page;
  GCRoot root_array[2];
  GCRootSet roots;
  bool evacuated;

  arena_init(&arena);

  first_root = (Pair*) arena_alloc_traced(&arena, sizeof(*first_root), &pair_trace);
  assert(first_root != NULL);
  assert(GC_STORE(&arena, first_root, left, NULL));
  assert(GC_STORE(&arena, first_root, right, NULL));
  first_page = arena_find_page(&arena, first_root);
  assert(first_page != NULL);

  while (arena.nursery_active_page == first_page) {
    void* filler = arena_alloc(&arena, 1024);
    assert(filler != NULL);
  }

  second_root = (Pair*) arena_alloc_traced(&arena, sizeof(*second_root), &pair_trace);
  assert(second_root != NULL);
  assert(GC_STORE(&arena, second_root, left, NULL));
  assert(GC_STORE(&arena, second_root, right, NULL));
  second_page = arena_find_page(&arena, second_root);
  assert(second_page != NULL);
  assert(second_page != first_page);

  root_array[0].slot = (GCPtr*) &first_root;
  root_array[1].slot = (GCPtr*) &second_root;
  roots.roots = root_array;
  roots.count = ARRAY_LEN(root_array);

  assert(gc_mark(&arena, &roots));
  gc_test_fail_forwarding_after(1);
  evacuated = gc_evacuate_sparse_pages(&arena, &roots);

  EXPECT_TRUE(!evacuated);
  EXPECT_TRUE(first_page->state != GC_PAGE_RELOCATING);
  EXPECT_TRUE(second_page->state != GC_PAGE_RELOCATING);
  EXPECT_TRUE(first_page->forwarding_count == 0);
  EXPECT_TRUE(second_page->forwarding_count == 0);
  EXPECT_TRUE(arena_find_page(&arena, first_root) == first_page);
  EXPECT_TRUE(arena_find_page(&arena, second_root) == second_page);
  EXPECT_TRUE(gc_stats(&arena)->copied_bytes == 0);

  TEST_LOG("multi_relocation_failure_test first_state=%d second_state=%d copied=%zu\n",
      (int) first_page->state,
      (int) second_page->state,
      gc_stats(&arena)->copied_bytes);

  arena_destroy(&arena);
}

static void test_collection_triggers_and_gc_alloc_young(void) {
  Arena arena;
  void* payload;
  Page* owner;
  size_t page_count_at_trigger;
  size_t initial_nursery_target;

  arena_init(&arena);
  initial_nursery_target = gc_policy(&arena)->nursery_page_target;

  while (arena_collection_trigger(&arena) != GC_TRIGGER_YOUNG) {
    payload = arena_alloc(&arena, 1024);
    assert(payload != NULL);
  }

  page_count_at_trigger = arena.page_count;
  EXPECT_TRUE(page_count_at_trigger >= GC_NURSERY_PAGE_TRIGGER);
  EXPECT_TRUE(arena_collection_trigger(&arena) == GC_TRIGGER_YOUNG);

  payload = gc_alloc(&arena, 1024, NULL);
  EXPECT_TRUE(payload != NULL);
  EXPECT_TRUE(arena.page_count == page_count_at_trigger);
  EXPECT_TRUE(arena_collection_trigger(&arena) == GC_TRIGGER_NONE);
  EXPECT_TRUE(gc_stats(&arena)->minor_collections == 1);
  EXPECT_TRUE(gc_stats(&arena)->last_collection_reason == GC_REASON_ALLOCATION_NURSERY_PRESSURE);
  EXPECT_TRUE(gc_policy(&arena)->nursery_page_target > initial_nursery_target);

  owner = arena_find_page(&arena, payload);
  EXPECT_TRUE(owner != NULL);
  EXPECT_TRUE(owner->space == GC_SPACE_NURSERY);
  EXPECT_TRUE(owner->age == GC_PAGE_AGE_YOUNG);

  TEST_LOG("trigger_young_test page_count=%zu owner_page=%d trigger=%d minor=%zu target=%zu\n",
      arena.page_count,
      arena_page_index(&arena, owner),
      (int) arena_collection_trigger(&arena),
      gc_stats(&arena)->minor_collections,
      gc_policy(&arena)->nursery_page_target);

  arena_destroy(&arena);
}

static void test_full_trigger_takes_precedence(void) {
  Arena arena;
  void* payload;
  ArenaCollectionTrigger trigger;

  arena_init(&arena);

  while ((GC_MAX_PAGES - arena.page_count) > GC_GC_PAGE_WATERMARK) {
    payload = arena_alloc(&arena, GC_LARGE_OBJECT_SIZE + 1024);
    assert(payload != NULL);
  }

  trigger = arena_collection_trigger(&arena);
  EXPECT_TRUE(trigger == GC_TRIGGER_FULL);
  EXPECT_TRUE(arena_should_collect(&arena));

  payload = gc_alloc(&arena, GC_LARGE_OBJECT_SIZE + 1024, NULL);
  EXPECT_TRUE(payload != NULL);
  EXPECT_TRUE(gc_stats(&arena)->full_collections == 1);
  EXPECT_TRUE(gc_stats(&arena)->last_collection_reason == GC_REASON_OLD_SPACE_PRESSURE);

  TEST_LOG("trigger_full_test page_count=%zu trigger=%d full=%zu reason=%d\n",
      arena.page_count,
      (int) trigger,
      gc_stats(&arena)->full_collections,
      (int) gc_stats(&arena)->last_collection_reason);

  arena_destroy(&arena);
}

static void test_old_allocation_minor_and_generic_slot_barrier(void) {
  Arena arena;
  Pair* parent;
  GCPtr parent_root;
  GCPtr child;
  GCPtr original_child;
  GCRoot root_array[1];
  GCRootSet roots;
  Page* parent_page;
  Page* child_page;

  arena_init(&arena);

  parent = (Pair*) gc_alloc_old_traced(&arena, sizeof(*parent), &pair_trace, NULL);
  assert(parent != NULL);
  assert(GC_STORE_SLOT(&arena, parent, &parent->left, NULL));
  assert(GC_STORE_SLOT(&arena, parent, &parent->right, NULL));

  parent_page = arena_find_page(&arena, parent);
  assert(parent_page != NULL);
  EXPECT_TRUE(parent_page->age == GC_PAGE_AGE_OLD);
  EXPECT_TRUE(parent_page->space == GC_SPACE_OLD);

  parent_root = parent;
  root_array[0].slot = &parent_root;
  roots.roots = root_array;
  roots.count = ARRAY_LEN(root_array);

  child = gc_alloc(&arena, 128, &roots);
  assert(child != NULL);
  original_child = child;
  child_page = arena_find_page(&arena, child);
  assert(child_page != NULL);
  EXPECT_TRUE(child_page->age == GC_PAGE_AGE_YOUNG);

  assert(GC_STORE_SLOT(&arena, parent, &parent->left, child));
  EXPECT_TRUE(gc_remembered_set_count(&arena) == 1);
  EXPECT_TRUE(gc_verify_remembered_set(&arena));

  assert(gc_collect_young(&arena, &roots));

  parent = (Pair*) parent_root;
  EXPECT_TRUE(parent != NULL);
  EXPECT_TRUE(parent->left != NULL);
  EXPECT_TRUE(parent->left != original_child);
  EXPECT_TRUE(arena_find_page(&arena, parent)->age == GC_PAGE_AGE_OLD);
  EXPECT_TRUE(arena_find_page(&arena, parent->left)->age == GC_PAGE_AGE_YOUNG);
  EXPECT_TRUE(gc_remembered_set_count(&arena) == 1);

  TEST_LOG("old_alloc_minor_test parent=%p child=%p remembered=%zu\n",
      (void*) parent,
      parent->left,
      gc_remembered_set_count(&arena));

  arena_destroy(&arena);
}

static void test_load_barrier_repairs_relocating_slot(void) {
  Arena arena;
  Pair* original;
  GCPtr slot;
  GCPtr loaded;
  Page* source_page;
  Page* loaded_page;

  arena_init(&arena);

  original = (Pair*) arena_alloc_traced(&arena, sizeof(*original), &pair_trace);
  assert(original != NULL);
  assert(GC_STORE(&arena, original, left, NULL));
  assert(GC_STORE(&arena, original, right, NULL));

  source_page = arena_find_page(&arena, original);
  assert(source_page != NULL);
  source_page->state = GC_PAGE_RELOCATING;
  slot = original;

  loaded = GC_LOAD_SLOT(&arena, &slot);
  loaded_page = arena_find_page(&arena, loaded);

  EXPECT_TRUE(loaded != NULL);
  EXPECT_TRUE(loaded != original);
  EXPECT_TRUE(slot == loaded);
  EXPECT_TRUE(loaded_page != NULL);
  EXPECT_TRUE(loaded_page != source_page);
  EXPECT_TRUE(((Pair*) loaded)->left == NULL);
  EXPECT_TRUE(((Pair*) loaded)->right == NULL);
  EXPECT_TRUE(source_page->forwarding_count == 1);

  gc_finish_relocation(&arena);
  gc_sweep(&arena);
  EXPECT_TRUE(source_page->state == GC_PAGE_FREE);
  EXPECT_TRUE(arena_find_page(&arena, loaded) == loaded_page);

  TEST_LOG("load_barrier_test old=%p new=%p source_state=%d\n",
      (void*) original,
      loaded,
      (int) source_page->state);

  arena_destroy(&arena);
}

static void test_load_barrier_failure_keeps_stale_slot_visible(void) {
  Arena arena;
  Pair* original;
  GCPtr slot;
  GCPtr loaded;
  Page* source_page;
  size_t page_count;
  ArenaGCStats stats;

  arena_init(&arena);

  original = (Pair*) arena_alloc_traced(&arena, sizeof(*original), &pair_trace);
  assert(original != NULL);
  assert(GC_STORE(&arena, original, left, NULL));
  assert(GC_STORE(&arena, original, right, NULL));

  source_page = arena_find_page(&arena, original);
  assert(source_page != NULL);
  source_page->state = GC_PAGE_RELOCATING;
  arena.nursery_active_page = NULL;
  page_count = arena.page_count;
  stats = *gc_stats(&arena);
  slot = original;

  gc_test_fail_forwarding_after(0);
  loaded = GC_LOAD_SLOT(&arena, &slot);

  EXPECT_TRUE(loaded == NULL);
  EXPECT_TRUE(slot == original);
  EXPECT_TRUE(arena.page_count == page_count);
  EXPECT_TRUE(arena.nursery_active_page == NULL);
  EXPECT_TRUE(source_page->forwarding_count == 0);
  EXPECT_TRUE(gc_stats(&arena)->copied_bytes == stats.copied_bytes);
  EXPECT_TRUE(gc_stats(&arena)->promoted_bytes == stats.promoted_bytes);

  source_page->state = GC_PAGE_ACTIVE;

  TEST_LOG("load_barrier_failure_test old=%p loaded=%p page_count=%zu forwarding=%zu\n",
      (void*) original,
      loaded,
      arena.page_count,
      source_page->forwarding_count);

  arena_destroy(&arena);
}

static void test_old_large_object_survives_minor_and_dies_in_full(void) {
  Arena arena;
  void* large;
  Page* large_page;

  arena_init(&arena);

  large = gc_alloc_old(&arena, GC_LARGE_OBJECT_SIZE + 512u, NULL);
  assert(large != NULL);
  large_page = arena_find_page(&arena, large);
  assert(large_page != NULL);
  EXPECT_TRUE(large_page->state == GC_PAGE_LARGE);
  EXPECT_TRUE(large_page->age == GC_PAGE_AGE_OLD);
  EXPECT_TRUE(large_page->space == GC_SPACE_LARGE);

  assert(gc_collect_young(&arena, NULL));
  EXPECT_TRUE(large_page->state == GC_PAGE_LARGE);
  EXPECT_TRUE(large_page->age == GC_PAGE_AGE_OLD);

  assert(gc_collect(&arena, NULL));
  EXPECT_TRUE(large_page->state == GC_PAGE_FREE);

  TEST_LOG("old_large_test page_state=%d minor=%zu full=%zu\n",
      (int) large_page->state,
      gc_stats(&arena)->minor_collections,
      gc_stats(&arena)->full_collections);

  arena_destroy(&arena);
}

static void test_gc_alloc_recovers_from_exhausted_unrooted_pages(void) {
  Arena arena;
  void* payload;
  size_t allocated_pages;

  arena_init(&arena);

  while (arena.page_count < GC_MAX_PAGES) {
    payload = arena_alloc(&arena, GC_LARGE_OBJECT_SIZE + 1024u);
    assert(payload != NULL);
  }

  allocated_pages = arena.page_count;
  EXPECT_TRUE(arena_collection_trigger(&arena) == GC_TRIGGER_FULL);

  payload = gc_alloc(&arena, GC_LARGE_OBJECT_SIZE + 512u, NULL);
  EXPECT_TRUE(payload != NULL);
  EXPECT_TRUE(allocated_pages == GC_MAX_PAGES);
  EXPECT_TRUE(gc_stats(&arena)->full_collections == 1);
  EXPECT_TRUE(gc_stats(&arena)->last_collection_reason == GC_REASON_OLD_SPACE_PRESSURE);
  EXPECT_TRUE(arena_find_page(&arena, payload) != NULL);

  TEST_LOG("alloc_retry_exhausted_test pages=%zu full=%zu payload=%p\n",
      allocated_pages,
      gc_stats(&arena)->full_collections,
      payload);

  arena_destroy(&arena);
}

static void test_bulk_allocation_iteration(void) {
  Arena arena;
  ObjectIterationStats stats;
  const size_t payload_size = 1024;

  arena_init(&arena);

  for (int i = 0; i < 1000; i++) {
    void* payload = arena_alloc(&arena, payload_size);
    assert(payload != NULL);

    TEST_LOG("alloc[%d] ptr=%p page_count=%zu nursery_active_page=%d survivor_active_page=%d old_active_page=%d\n",
        i,
        payload,
        arena.page_count,
        arena_page_index(&arena, arena.nursery_active_page),
        arena_page_index(&arena, arena.survivor_active_page),
        arena_page_index(&arena, arena.old_active_page));
  }

  stats.objects = 0;
  stats.bytes = 0;
  arena_for_each_object(&arena, count_object, &stats);

  TEST_LOG("iterated objects=%zu bytes=%zu\n", stats.objects, stats.bytes);
  EXPECT_TRUE(stats.objects == 1000);
  EXPECT_TRUE(stats.bytes == 1000u * arena_make_layout(payload_size).total_size);
  EXPECT_TRUE(arena_should_collect(&arena));

  arena_destroy(&arena);
}

int main(void) {
  const size_t payload_size = 1024;

  RUN_ARENA_TEST("page livemap mark", test_page_livemap_mark(&arena, payload_size));
  RUN_ARENA_TEST("root mark", test_root_mark(&arena, payload_size));
  RUN_ARENA_TEST("object field mark", test_object_field_mark(&arena, payload_size));
  RUN_ARENA_TEST("transitive root mark", test_transitive_root_mark(&arena, payload_size));
  RUN_TEST("forward relocating object", test_gc_forward_if_relocating());
  RUN_TEST("full collect evacuates sparse page", test_gc_collect_evacuates_sparse_page());
  RUN_TEST("reuse free normal page", test_reuse_free_normal_page());
  RUN_TEST("sweep dead normal pages", test_sweep_dead_normal_pages());
  RUN_TEST("sweep and reuse large page", test_sweep_and_reuse_large_page());
  RUN_TEST("promote surviving page", test_promote_surviving_page());
  RUN_TEST("minor collect old to young", test_minor_collect_old_to_young());
  RUN_TEST("minor promoted parent remembers child", test_minor_promoted_parent_remembers_young_child());
  RUN_TEST("minor repairs young fields", test_minor_repair_young_to_young_fields());
  RUN_TEST("minor repairs remembered old slot", test_minor_repairs_remembered_slot_only_in_old_object());
  RUN_TEST("barrier failure rolls back slot", test_write_barrier_failure_rolls_back_slot());
  RUN_TEST("barrier failure preserves slot", test_write_barrier_failure_preserves_existing_slot());
  RUN_TEST(
      "remembered verification detects missing barrier",
      test_remembered_set_verification_detects_missing_barrier());
  RUN_TEST("remembered sets are page local", test_remembered_sets_are_page_local_and_deduplicated());
  RUN_TEST("remembered set prunes stale slots", test_remembered_set_prunes_stale_slots());
  RUN_TEST("remembered set stress repair", test_remembered_set_stress_minor_relocation_repair());
  RUN_TEST("full collect rebuilds remembered sets", test_full_collect_rebuilds_remembered_sets());
  RUN_TEST("root registry failure and dedup", test_root_registration_failure_and_deduplication());
  RUN_TEST("registered root survives full collect", test_registered_root_survives_full_collect());
  RUN_TEST("unregistered root allows collection", test_unregistered_root_allows_collection());
  RUN_TEST("handle survives minor collect", test_handle_survives_minor_collect());
  RUN_TEST("oversized allocation rejected", test_oversized_allocation_is_rejected());
  RUN_TEST("oversized allocation preserves arena", test_oversized_allocation_preserves_existing_arena());
  RUN_TEST("invalid trace descriptor rejected", test_invalid_trace_descriptor_is_rejected());
  RUN_TEST("failed relocation preserves destination", test_failed_relocation_preserves_destination_page());
  RUN_TEST(
      "failed relocation releases new destination",
      test_failed_relocation_releases_new_destination_page());
  RUN_TEST(
      "failed multi-page relocation rolls back",
      test_failed_multi_page_relocation_rolls_back_all_sources());
  RUN_TEST("allocation triggers young collect", test_collection_triggers_and_gc_alloc_young());
  RUN_TEST("full trigger takes precedence", test_full_trigger_takes_precedence());
  RUN_TEST(
      "old allocation minor slot barrier",
      test_old_allocation_minor_and_generic_slot_barrier());
  RUN_TEST("load barrier repairs relocating slot", test_load_barrier_repairs_relocating_slot());
  RUN_TEST(
      "load barrier failure is explicit",
      test_load_barrier_failure_keeps_stale_slot_visible());
  RUN_TEST("old large object minor/full", test_old_large_object_survives_minor_and_dies_in_full());
  RUN_TEST(
      "gc alloc recovers exhausted pages",
      test_gc_alloc_recovers_from_exhausted_unrooted_pages());
  RUN_TEST("bulk allocation iteration", test_bulk_allocation_iteration());

  printf("\nSummary: %zu tests, %zu failed test%s, %zu failed assertion%s\n",
      test_count,
      failed_test_count,
      failed_test_count == 1u ? "" : "s",
      test_failures,
      test_failures == 1u ? "" : "s");

  if (test_failures > 0) {
    return 1;
  }

  return 0;
}

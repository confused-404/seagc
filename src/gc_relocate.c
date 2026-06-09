#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "livemap.h"
#include "macros.h"
#include "object_header.h"

enum {
  GC_RELOCATION_LIVE_RATIO_SHIFT = 2,
};

typedef struct PageList {
  Page** items;
  size_t count;
  size_t capacity;
} PageList;

typedef struct RelocationPlan {
  const ObjectHeader* old_header;
  AllocLayout layout;
  size_t old_offset;
  PageAge destination_age;
  u8 object_age;
} RelocationPlan;

static bool gc_test_forwarding_failure_enabled;
static size_t gc_test_forwarding_successes_before_failure;
static size_t gc_test_forwarding_successes;

void gc_test_fail_forwarding_after(size_t successful_forwarding_entries) {
  gc_test_forwarding_failure_enabled = true;
  gc_test_forwarding_successes_before_failure = successful_forwarding_entries;
  gc_test_forwarding_successes = 0;
}

static void gc_assert_relocation_page(const Page* page) {
  assert(page->state == GC_PAGE_RELOCATING);
}

static void gc_assert_nonrelocating_page(const Page* page) {
  assert(page->state != GC_PAGE_RELOCATING);
}

static void gc_assert_relocation_destination_page(const Page* page, size_t min_capacity) {
  assert(page != NULL);
  assert(page->state == GC_PAGE_ACTIVE);
  gc_assert_nonrelocating_page(page);
  assert(page->base != NULL);
  assert(page->capacity >= min_capacity);
  assert(page->forwarding_count == 0);
}

static PageForwardingEntry* page_find_forwarding(Page* page, size_t old_offset) {
  for (size_t i = 0; i < page->forwarding_count; i++) {
    if (page->forwarding[i].old_offset == old_offset) {
      return &page->forwarding[i];
    }
  }

  return NULL;
}

static bool page_list_push(PageList* list, Page* page) {
  Page** items;
  size_t new_capacity;

  for (size_t i = 0; i < list->count; i++) {
    if (list->items[i] == page) {
      return true;
    }
  }

  if (list->count == list->capacity) {
    new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
    items = (Page**) realloc(list->items, new_capacity * sizeof(list->items[0]));
    if (items == NULL) {
      return false;
    }

    list->items = items;
    list->capacity = new_capacity;
  }

  list->items[list->count++] = page;
  return true;
}

static void page_list_reset(PageList* list) {
  free(list->items);
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

static bool page_add_forwarding(Page* page, size_t old_offset, u8* new_payload) {
  PageForwardingEntry* entries;
  size_t new_capacity;

  gc_assert_relocation_page(page);

  if (gc_test_forwarding_failure_enabled &&
      gc_test_forwarding_successes >= gc_test_forwarding_successes_before_failure) {
    gc_test_forwarding_failure_enabled = false;
    return false;
  }

  if (page->forwarding_count == page->forwarding_capacity) {
    new_capacity = page->forwarding_capacity == 0 ? 8 : page->forwarding_capacity * 2;
    entries = (PageForwardingEntry*) realloc(page->forwarding, new_capacity * sizeof(page->forwarding[0]));
    if (entries == NULL) {
      return false;
    }

    page->forwarding = entries;
    page->forwarding_capacity = new_capacity;
  }

  page->forwarding[page->forwarding_count].old_offset = old_offset;
  page->forwarding[page->forwarding_count].new_payload = new_payload;
  page->forwarding_count++;
  if (gc_test_forwarding_failure_enabled) {
    gc_test_forwarding_successes++;
  }
  return true;
}

static bool gc_page_is_relocation_candidate(const Page* page) {
  if (page->state != GC_PAGE_ACTIVE && page->state != GC_PAGE_FULL) {
    return false;
  }

  return page->livemap.live_bytes > 0 &&
      page->livemap.live_bytes <= (page->capacity >> GC_RELOCATION_LIVE_RATIO_SHIFT);
}

static RelocationPlan gc_make_relocation_plan(
    Page* source_page,
    size_t old_offset,
    bool age_young_object) {
  RelocationPlan plan;
  u8 next_age;

  gc_assert_relocation_page(source_page);

  plan.old_header = (const ObjectHeader*) (source_page->base + old_offset);
  plan.layout = arena_make_layout(plan.old_header->size);
  plan.old_offset = old_offset;
  plan.destination_age = source_page->age;
  plan.object_age = plan.old_header->age;

  if (age_young_object && source_page->age == GC_PAGE_AGE_YOUNG) {
    next_age = plan.old_header->age == UINT8_MAX ? UINT8_MAX : (u8) (plan.old_header->age + 1);
    plan.object_age = next_age;
    if (next_age >= GC_PROMOTION_AGE) {
      plan.destination_age = GC_PAGE_AGE_OLD;
    }
  }

  return plan;
}

static Page* gc_acquire_relocation_destination_page(Arena* arena, size_t min_capacity, PageAge age) {
  Page* destination_page = arena_get_active_page_for_age(arena, min_capacity, age);

  if (destination_page == NULL) {
    return NULL;
  }

  gc_assert_relocation_destination_page(destination_page, min_capacity);
  return destination_page;
}

static bool gc_forward_live_object(
    Arena* arena,
    Page* source_page,
    size_t old_offset,
    bool age_young_object,
    void** new_payload_out,
    Page** destination_page_out) {
  const size_t header_size = arena_make_layout(0).header_size;
  const RelocationPlan plan = gc_make_relocation_plan(source_page, old_offset, age_young_object);
  Page* destination_page;
  ObjectHeader* new_header;
  u8* new_top;
  size_t dest_offset;

  if (plan.layout.total_size > source_page->capacity) {
    return false;
  }

  destination_page = gc_acquire_relocation_destination_page(
      arena,
      plan.layout.total_size,
      plan.destination_age);
  if (destination_page == NULL) {
    return false;
  }

  new_top = destination_page->top;
  dest_offset = (size_t) (new_top - destination_page->base);

  if (!page_add_forwarding(source_page, old_offset, new_top + header_size)) {
    return false;
  }

  if (!livemap_mark(&destination_page->livemap, dest_offset, plan.layout.total_size)) {
    source_page->forwarding_count--;
    return false;
  }

  new_header = (ObjectHeader*) new_top;
  memcpy(new_header, plan.old_header, plan.layout.total_size);
  new_header->age = plan.object_age;
  destination_page->top += plan.layout.total_size;
  destination_page->used += plan.layout.total_size;

  *new_payload_out = (void*) (new_top + header_size);
  if (destination_page_out != NULL) {
    *destination_page_out = destination_page;
  }
  return true;
}

void* gc_forward_if_relocating(Arena* arena, void* object) {
  Page* source_page;
  const size_t header_size = arena_make_layout(0).header_size;
  const ObjectHeader* hp;
  size_t old_offset;
  PageForwardingEntry* entry;
  void* new_payload;

  if (object == NULL) {
    return NULL;
  }

  source_page = arena_find_page(arena, object);
  if (source_page == NULL) {
    return object;
  }

  hp = get_header_pointer(object, header_size);
  old_offset = (size_t) ((const u8*) hp - source_page->base);
  entry = page_find_forwarding(source_page, old_offset);
  if (entry != NULL) {
    return entry->new_payload;
  }

  if (source_page->state != GC_PAGE_RELOCATING) {
    return object;
  }

  if (!gc_forward_live_object(arena, source_page, old_offset, false, &new_payload, NULL)) {
    return NULL;
  }

  return new_payload;
}

void* gc_forward_existing_if_relocating(Arena* arena, void* object) {
  Page* source_page;
  const size_t header_size = arena_make_layout(0).header_size;
  const ObjectHeader* hp;
  size_t old_offset;
  PageForwardingEntry* entry;

  if (object == NULL) {
    return NULL;
  }

  source_page = arena_find_page(arena, object);
  if (source_page == NULL) {
    return object;
  }

  if (source_page->state != GC_PAGE_RELOCATING) {
    return object;
  }

  hp = get_header_pointer(object, header_size);
  old_offset = (size_t) ((const u8*) hp - source_page->base);
  entry = page_find_forwarding(source_page, old_offset);
  if (entry == NULL) {
    return NULL;
  }

  return entry->new_payload;
}

static void gc_cleanup_failed_relocation(Arena* arena, Page* source_page, PageList* destinations) {
  (void) arena;

  for (size_t i = 0; i < destinations->count; i++) {
    Page* page = destinations->items[i];
    page_reset(page, GC_PAGE_FREE, GC_PAGE_AGE_YOUNG);
  }

  source_page->state = GC_PAGE_FULL;
  page_clear_forwarding(source_page);
  page_list_reset(destinations);
}

static bool gc_evacuate_page(Arena* arena, Page* source_page, bool age_young_objects) {
  PageList destinations = {
    .items = NULL,
    .count = 0,
    .capacity = 0,
  };
  source_page->state = GC_PAGE_RELOCATING;
  assert(source_page->forwarding_count == 0);

  for (u8* cursor = source_page->base; cursor < source_page->top; ) {
    ObjectHeader* header = (ObjectHeader*) cursor;
    size_t old_offset = (size_t) (cursor - source_page->base);
    u8* new_payload;
    Page* destination_page = NULL;

    if (livemap_is_live(&source_page->livemap, old_offset)) {
      if (!gc_forward_live_object(
          arena,
          source_page,
          old_offset,
          age_young_objects,
          (void**) &new_payload,
          &destination_page)) {
        gc_cleanup_failed_relocation(arena, source_page, &destinations);
        return false;
      }

      if (destination_page != NULL && !page_list_push(&destinations, destination_page)) {
        gc_cleanup_failed_relocation(arena, source_page, &destinations);
        return false;
      }
    }

    cursor += header->total_size;
  }

  page_list_reset(&destinations);
  return true;
}

bool gc_verify_relocation(Arena* arena) {
  const size_t header_size = arena_make_layout(0).header_size;

  for (size_t i = 0; i < arena->page_count; i++) {
    Page* source_page = &arena->pages[i];

    if (source_page->state != GC_PAGE_RELOCATING) {
      continue;
    }

    for (size_t j = 0; j < source_page->forwarding_count; j++) {
      const PageForwardingEntry* entry = &source_page->forwarding[j];
      const ObjectHeader* old_header = (const ObjectHeader*) (source_page->base + entry->old_offset);
      Page* destination_page = arena_find_page(arena, entry->new_payload);
      const ObjectHeader* new_header;
      size_t destination_offset;

      if (destination_page == NULL || destination_page == source_page) {
        return false;
      }

      new_header = get_header_pointer(entry->new_payload, header_size);
      destination_offset = (size_t) ((const u8*) new_header - destination_page->base);

      if (new_header->size != old_header->size) {
        return false;
      }

      if (!livemap_is_live(&destination_page->livemap, destination_offset)) {
        return false;
      }
    }
  }

  return true;
}

void gc_finish_relocation(Arena* arena) {
  for (size_t i = 0; i < arena->page_count; i++) {
    Page* page = &arena->pages[i];

    if (page->state == GC_PAGE_RELOCATING) {
      assert(page->forwarding_count == 0 || page->forwarding != NULL);
      if (arena->young_active_page == page) {
        arena->young_active_page = NULL;
      }
      if (arena->old_active_page == page) {
        arena->old_active_page = NULL;
      }
      page_reset(page, GC_PAGE_FREE, GC_PAGE_AGE_YOUNG);
    }
  }
}

bool gc_evacuate_sparse_pages(Arena* arena, const GCRootSet* roots) {
  (void) roots;

  const size_t initial_page_count = arena->page_count;

  for (size_t i = 0; i < initial_page_count; i++) {
    Page* source_page = &arena->pages[i];

    if (!gc_page_is_relocation_candidate(source_page)) {
      continue;
    }

    if (!gc_evacuate_page(arena, source_page, false)) {
      return false;
    }
  }

  return true;
}

bool gc_evacuate_young_pages(Arena* arena, const GCRootSet* roots) {
  const size_t initial_page_count = arena->page_count;

  (void) roots;

  for (size_t i = 0; i < initial_page_count; i++) {
    Page* source_page = &arena->pages[i];

    if (source_page->age != GC_PAGE_AGE_YOUNG) {
      continue;
    }

    if (source_page->state == GC_PAGE_FREE || source_page->livemap.live_objects == 0) {
      continue;
    }

    if (source_page->state == GC_PAGE_LARGE) {
      page_promote(source_page);
      continue;
    }

    if (source_page == arena->young_active_page) {
      arena->young_active_page = NULL;
    }
    if (source_page == arena->old_active_page) {
      arena->old_active_page = NULL;
    }

    if (!gc_evacuate_page(arena, source_page, true)) {
      return false;
    }
  }

  return true;
}

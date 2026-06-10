#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "config.h"
#include "livemap.h"
#include "macros.h"
#include "object_header.h"

static size_t align_size(size_t size) {
  return ALIGN_UP(size, GC_ALIGNMENT);
}

static bool checked_align_size(size_t size, size_t* aligned_size) {
  const size_t alignment_mask = GC_ALIGNMENT - 1u;

  if (size > SIZE_MAX - alignment_mask) {
    return false;
  }

  *aligned_size = ALIGN_UP(size, GC_ALIGNMENT);
  return true;
}

static const TraceDescriptor gc_trace_none = {
  .pointer_count = 0,
  .pointer_offsets = NULL,
};

static size_t get_page_offset(const Page* page, const ObjectHeader* hp) {
  const u8* h_start = (const u8*) hp;

  return (size_t) (h_start - (const u8*) page->base);
}

static size_t get_header_size(void) {
  return align_size(sizeof(ObjectHeader));
}

static bool arena_make_layout_checked(size_t payload_size, AllocLayout* alloc_layout) {
  size_t aligned_payload_size;
  size_t unaligned_total_size;

  alloc_layout->header_size = get_header_size();

  if (!checked_align_size(payload_size, &aligned_payload_size)) {
    alloc_layout->total_size = 0;
    return false;
  }

  if (alloc_layout->header_size > SIZE_MAX - aligned_payload_size) {
    alloc_layout->total_size = 0;
    return false;
  }

  unaligned_total_size = alloc_layout->header_size + aligned_payload_size;
  if (!checked_align_size(unaligned_total_size, &alloc_layout->total_size)) {
    alloc_layout->total_size = 0;
    return false;
  }

  return true;
}

AllocLayout arena_make_layout(size_t payload_size) {
  AllocLayout alloc_layout;

  (void) arena_make_layout_checked(payload_size, &alloc_layout);
  return alloc_layout;
}

static bool arena_trace_descriptor_is_valid(
    const TraceDescriptor* trace,
    size_t payload_size) {
  if (trace == NULL || trace->pointer_count == 0) {
    return true;
  }

  if (trace->pointer_offsets == NULL) {
    return false;
  }

  for (size_t i = 0; i < trace->pointer_count; i++) {
    const size_t offset = trace->pointer_offsets[i];

    if (offset > payload_size || payload_size - offset < sizeof(void*)) {
      return false;
    }
  }

  return true;
}

Page* arena_add_page(Arena* arena, size_t capacity, PageState state, PageAge age) {
  Page* page = NULL;

  if (arena->page_count >= GC_MAX_PAGES) {
    return NULL;
  }

  page = &arena->pages[arena->page_count++];
  page->base = calloc(1, capacity);
  if (page->base == NULL) {
    arena->page_count--;
    return NULL;
  }

  page_init(page, page->base, capacity, state, age);

  return page;
}

static Page** arena_active_page_slot(Arena* arena, PageAge age) {
  switch (age) {
    case GC_PAGE_AGE_YOUNG:
      return &arena->young_active_page;
    case GC_PAGE_AGE_OLD:
      return &arena->old_active_page;
    default:
      assert(false);
      return &arena->young_active_page;
  }
}

Page* arena_get_active_page_for_age(Arena* arena, size_t size, PageAge age) {
  Page** active_page = arena_active_page_slot(arena, age);
  Page* page;

  if (*active_page != NULL) {
    if ((*active_page)->state != GC_PAGE_ACTIVE || (*active_page)->age != age) {
      *active_page = NULL;
    }
  }

  if (*active_page != NULL) {
    size_t remaining = (size_t) ((*active_page)->limit - (*active_page)->top);
    if (remaining >= size) {
      return *active_page;
    }
    (*active_page)->state = GC_PAGE_FULL;
    *active_page = NULL;
  }

  for (size_t i = 0; i < arena->page_count; i++) {
    page = &arena->pages[i];

    if (page->state == GC_PAGE_FREE && page->base != NULL && page->capacity == GC_PAGE_SIZE) {
      assert(page->forwarding_count == 0);
      page_reset(page, GC_PAGE_ACTIVE, age);
      *active_page = page;
      return page;
    }
  }

  page = arena_add_page(arena, GC_PAGE_SIZE, GC_PAGE_ACTIVE, age);
  if (page == NULL) {
    return NULL;
  }

  *active_page = page;
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

  free(arena->remembered_set.slots);
  arena->remembered_set.slots = NULL;
  arena->remembered_set.count = 0;
  arena->remembered_set.capacity = 0;
  free(arena->roots.slots);
  arena->roots.slots = NULL;
  arena->roots.count = 0;
  arena->roots.capacity = 0;
  arena->page_count = 0;
  arena->young_active_page = NULL;
  arena->old_active_page = NULL;
}

static void* arena_alloc_large(Arena* arena, const ObjectHeader* header, const AllocLayout* alloc_layout) {
  Page* page = NULL;
  u8* top;
  ObjectHeader* h_dest;

  for (size_t i = 0; i < arena->page_count; i++) {
    Page* candidate = &arena->pages[i];

    if (candidate->state == GC_PAGE_FREE &&
        candidate->base != NULL &&
        candidate->capacity >= alloc_layout->total_size) {
      page = candidate;
      page_reset(page, GC_PAGE_LARGE, GC_PAGE_AGE_YOUNG);
      break;
    }
  }

  if (page == NULL) {
    page = arena_add_page(arena, alloc_layout->total_size, GC_PAGE_LARGE, GC_PAGE_AGE_YOUNG);
  }

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

  page = arena_get_active_page_for_age(
      arena,
      alloc_layout->total_size,
      GC_PAGE_AGE_YOUNG);
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

void* arena_alloc_traced(Arena* arena, size_t payload_size, const TraceDescriptor* trace) {
  ObjectHeader header;
  AllocLayout alloc_layout;

  if (!arena_make_layout_checked(payload_size, &alloc_layout)) {
    return NULL;
  }

  if (!arena_trace_descriptor_is_valid(trace, payload_size)) {
    return NULL;
  }

  header.size = payload_size;
  header.total_size = alloc_layout.total_size;
  header.trace = trace != NULL ? trace : &gc_trace_none;
  header.age = 0;

  if (alloc_layout.total_size > GC_LARGE_OBJECT_SIZE) {
    return arena_alloc_large(arena, &header, &alloc_layout);
  }

  return arena_alloc_normal(arena, &header, &alloc_layout);
}

void* arena_alloc(Arena* arena, size_t payload_size) {
  return arena_alloc_traced(arena, payload_size, &gc_trace_none);
}

bool arena_should_collect(const Arena* arena) {
  return (GC_MAX_PAGES - arena->page_count) <= GC_GC_PAGE_WATERMARK;
}

Page* arena_find_page(Arena* arena, const void* payload_pointer) {
  const size_t header_size = get_header_size();
  const u8* hp = (const u8*) get_header_pointer(payload_pointer, header_size);

  for (size_t i = 0; i < arena->page_count; i++) {
    const u8* ps = arena->pages[i].base;
    const u8* pe = arena->pages[i].limit;

    if (arena->pages[i].state == GC_PAGE_FREE) {
      continue;
    }

    if (hp >= ps && hp < pe) {
      return &arena->pages[i];
    }
  }

  return NULL;
}

bool arena_mark_object(Arena* arena, const void* payload_pointer) {
  const size_t header_size = get_header_size();
  Page* page = arena_find_page(arena, payload_pointer);
  if (page == NULL) {
    return false;
  }
  const ObjectHeader* hp = get_header_pointer(payload_pointer, header_size);

  size_t page_offset = get_page_offset(page, hp);

  return livemap_mark(&page->livemap, page_offset, hp->total_size);
}

bool arena_visit_object_fields(
    Arena* arena,
    void* payload_pointer,
    ArenaObjectFieldVisitor visitor,
    void* user_data) {
  const size_t header_size = get_header_size();
  const ObjectHeader* hp;
  const TraceDescriptor* trace;

  assert(visitor != NULL);

  if (payload_pointer == NULL) {
    return true;
  }

  if (arena_find_page(arena, payload_pointer) == NULL) {
    return true;
  }

  hp = get_header_pointer(payload_pointer, header_size);
  trace = hp->trace;
  if (trace == NULL) {
    return true;
  }

  for (size_t i = 0; i < trace->pointer_count; i++) {
    const size_t offset = trace->pointer_offsets[i];
    void** field = (void**) ((u8*) payload_pointer + offset);

    if (!visitor(hp, payload_pointer, field, user_data)) {
      return false;
    }
  }

  return true;
}

static bool arena_mark_field_visitor(
    const ObjectHeader* header,
    void* payload,
    void** field_slot,
    void* user_data) {
  Arena* arena = (Arena*) user_data;

  (void) header;
  (void) payload;

  if (*field_slot != NULL) {
    (void) arena_mark_object(arena, *field_slot);
  }

  return true;
}

void arena_mark_object_fields(Arena* arena, void* payload_pointer) {
  (void) arena_visit_object_fields(
      arena,
      payload_pointer,
      arena_mark_field_visitor,
      arena);
}

void arena_for_each_object(Arena* arena, ArenaObjectVisitor visitor, void* user_data) {
  const size_t header_size = get_header_size();

  for (size_t i = 0; i < arena->page_count; i++) {
    Page* page = &arena->pages[i];
    u8* cursor = page->base;

    while (cursor < page->top) {
      const ObjectHeader* header = (const ObjectHeader*) cursor;
      void* payload = (void*) (cursor + header_size);

      assert(header->total_size >= header_size);
      assert(cursor + header->total_size <= page->top);

      visitor(page, header, payload, user_data);
      cursor += header->total_size;
    }
  }
}

const ObjectHeader* get_header_pointer(const void* payload_pointer, size_t header_size) {
  return (const ObjectHeader*) ((const u8*) payload_pointer - header_size);
}

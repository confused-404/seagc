#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "livemap.h"
#include "macros.h"
#include "object_header.h"

typedef struct MarkWorklist {
  GCPtr* items;
  size_t count;
  size_t capacity;
} MarkWorklist;

enum {
  GC_RELOCATION_LIVE_RATIO_SHIFT = 2,
};

static PageForwardingEntry* page_find_forwarding(Page* page, size_t old_offset) {
  for (size_t i = 0; i < page->forwarding_count; i++) {
    if (page->forwarding[i].old_offset == old_offset) {
      return &page->forwarding[i];
    }
  }

  return NULL;
}

static bool page_add_forwarding(Page* page, size_t old_offset, u8* new_payload) {
  PageForwardingEntry* entries;
  size_t new_capacity;

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
  return true;
}

static bool gc_repair_pointer(Arena* arena, GCPtr* slot);
static void gc_repair_object_fields(Arena* arena, void* payload_pointer, bool* ok);
typedef struct RepairVisitState {
  Arena* arena;
  bool ok;
} RepairVisitState;

static void gc_repair_visit(Page* page, const ObjectHeader* header, void* payload, void* user_data) {
  RepairVisitState* state = (RepairVisitState*) user_data;
  (void) page;
  (void) header;
  gc_repair_object_fields(state->arena, payload, &state->ok);
}

static Page* arena_get_free_normal_page(Arena* arena, size_t min_capacity) {
  for (size_t i = 0; i < arena->page_count; i++) {
    Page* page = &arena->pages[i];
    if (page->state == GC_PAGE_FREE && page->base != NULL && page->capacity >= min_capacity) {
      page_reset(page, GC_PAGE_ACTIVE);
      return page;
    }
  }
  return NULL;
}

static bool gc_repair_pointer(Arena* arena, GCPtr* slot) {
  if (slot == NULL || *slot == NULL) {
    return true;
  }

  *slot = gc_forward_object(arena, *slot);
  return *slot != NULL;
}

static void gc_repair_object_fields(Arena* arena, void* payload_pointer, bool* ok) {
  const size_t header_size = arena_make_layout(0).header_size;
  const ObjectHeader* hp;
  const TraceDescriptor* trace;

  if (!*ok || payload_pointer == NULL) {
    return;
  }

  hp = get_header_pointer(payload_pointer, header_size);
  trace = hp->trace;
  if (trace == NULL) {
    return;
  }

  for (size_t i = 0; i < trace->pointer_count; i++) {
    GCPtr* field = (GCPtr*) ((u8*) payload_pointer + trace->pointer_offsets[i]);
    *ok = gc_repair_pointer(arena, field);
    if (!*ok) {
      return;
    }
  }
}

static bool gc_forward_live_object(Arena* arena, Page* source_page, size_t old_offset, void** new_payload_out) {
  const size_t header_size = arena_make_layout(0).header_size;
  const ObjectHeader* old_header = (const ObjectHeader*) (source_page->base + old_offset);
  const AllocLayout layout = arena_make_layout(old_header->size);
  Page* destination_page;
  ObjectHeader* new_header;
  u8* new_top;
  size_t dest_offset;

  if (layout.total_size > source_page->capacity) {
    return false;
  }

  destination_page = arena_get_free_normal_page(arena, layout.total_size);
  if (destination_page == NULL) {
    for (size_t i = 0; i < arena->page_count; i++) {
      Page* candidate = &arena->pages[i];

      if (candidate->state == GC_PAGE_ACTIVE &&
          candidate != source_page &&
          candidate->capacity == GC_PAGE_SIZE) {
        size_t remaining = (size_t) (candidate->limit - candidate->top);

        if (remaining >= layout.total_size) {
          destination_page = candidate;
          break;
        }
      }
    }
  }

  if (destination_page == NULL) {
    destination_page = arena_add_page(arena, GC_PAGE_SIZE, GC_PAGE_ACTIVE);
  }

  if (destination_page->state != GC_PAGE_ACTIVE) {
    return false;
  }

  new_top = destination_page->top;
  dest_offset = (size_t) (new_top - destination_page->base);

  if (!page_add_forwarding(source_page, old_offset, new_top + header_size)) {
    return false;
  }

  if (!livemap_mark(&destination_page->livemap, dest_offset, layout.total_size)) {
    source_page->forwarding_count--;
    return false;
  }

  new_header = (ObjectHeader*) new_top;
  memcpy(new_header, old_header, layout.total_size);
  destination_page->top += layout.total_size;
  destination_page->used += layout.total_size;

  *new_payload_out = (void*) (new_top + header_size);
  return true;
}

void* gc_forward_object(Arena* arena, void* object) {
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

  if (!gc_forward_live_object(arena, source_page, old_offset, &new_payload)) {
    return NULL;
  }

  return new_payload;
}

static bool gc_repair_roots(Arena* arena, const GCRootSet* roots) {
  if (roots == NULL) {
    return true;
  }

  for (size_t i = 0; i < roots->count; i++) {
    const GCRoot* root = &roots->roots[i];
    if (!gc_repair_pointer(arena, root->slot)) {
      return false;
    }
  }

  return true;
}

static bool gc_repair_all_objects(Arena* arena) {
  RepairVisitState state = {
    .arena = arena,
    .ok = true,
  };

  arena_for_each_object(arena, gc_repair_visit, &state);
  return state.ok;
}

static void gc_finish_relocation(Arena* arena) {
  for (size_t i = 0; i < arena->page_count; i++) {
    Page* page = &arena->pages[i];

    if (page->state == GC_PAGE_RELOCATING) {
      page_reset(page, GC_PAGE_FREE);
    }
  }
}

static bool gc_page_is_sparse(Page* page) {
  if (page->state != GC_PAGE_ACTIVE && page->state != GC_PAGE_FULL) {
    return false;
  }

  return page->livemap.live_bytes > 0 &&
      page->livemap.live_bytes <= (page->capacity >> GC_RELOCATION_LIVE_RATIO_SHIFT);
}

static bool gc_verify_relocation(Arena* arena) {
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

bool gc_evacuate_sparse_pages(Arena* arena, const GCRootSet* roots) {
  (void) roots;

  const size_t initial_page_count = arena->page_count;

  for (size_t i = 0; i < initial_page_count; i++) {
    Page* source_page = &arena->pages[i];

    if (!gc_page_is_sparse(source_page)) {
      continue;
    }

    source_page->state = GC_PAGE_RELOCATING;

    for (u8* cursor = source_page->base; cursor < source_page->top; ) {
      ObjectHeader* header = (ObjectHeader*) cursor;
      size_t old_offset = (size_t) (cursor - source_page->base);
      u8* new_payload;

      if (livemap_is_live(&source_page->livemap, old_offset)) {
        if (!gc_forward_live_object(arena, source_page, old_offset, (void**) &new_payload)) {
          source_page->state = GC_PAGE_FULL;
          return false;
        }
      }

      cursor += header->total_size;
    }
  }

  return true;
}

void* gc_alloc(Arena* arena, size_t payload_size, const GCRootSet* roots) {
  return gc_alloc_traced(arena, payload_size, NULL, roots);
}

void* gc_alloc_traced(
    Arena* arena,
    size_t payload_size,
    const TraceDescriptor* trace,
    const GCRootSet* roots) {
  void* payload = arena_alloc_traced(arena, payload_size, trace);

  if (payload != NULL) {
    return payload;
  }

  if (!gc_collect(arena, roots)) {
    return NULL;
  }

  return arena_alloc_traced(arena, payload_size, trace);
}

static void mark_worklist_destroy(MarkWorklist* worklist) {
  free(worklist->items);
  worklist->items = NULL;
  worklist->count = 0;
  worklist->capacity = 0;
}

static bool mark_worklist_push(MarkWorklist* worklist, GCPtr item) {
  if (worklist->count == worklist->capacity) {
    size_t new_capacity;
    GCPtr* new_items;

    if (worklist->capacity > SIZE_MAX / 2) {
      return false;
    }

    new_capacity = worklist->capacity == 0 ? 16 : worklist->capacity * 2;

    if (new_capacity > SIZE_MAX / sizeof(worklist->items[0])) {
      return false;
    }

    new_items = (GCPtr*) realloc(worklist->items, new_capacity * sizeof(worklist->items[0]));
    if (new_items == NULL) {
      return false;
    }

    worklist->items = new_items;
    worklist->capacity = new_capacity;
  }

  worklist->items[worklist->count++] = item;
  return true;
}

static GCPtr mark_worklist_pop(MarkWorklist* worklist) {
  assert(worklist->count > 0);
  return worklist->items[--worklist->count];
}

static bool mark_object_fields_into_worklist(
    Arena* arena,
    const void* payload_pointer,
    MarkWorklist* worklist) {
  const size_t header_size = arena_make_layout(0).header_size;
  const ObjectHeader* hp;
  const TraceDescriptor* trace;

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
    const GCPtr* field = (const GCPtr*) ((const u8*) payload_pointer + offset);

    if (*field != NULL && arena_mark_object(arena, *field)) {
      if (!mark_worklist_push(worklist, *field)) {
        return false;
      }
    }
  }

  return true;
}

bool gc_mark_roots(Arena* arena, const GCRootSet* roots) {
  MarkWorklist worklist = {
    .items = NULL,
    .count = 0,
    .capacity = 0,
  };
  bool ok = true;

  if (roots == NULL) {
    return true;
  }

  for (size_t i = 0; i < roots->count; i++) {
    const GCRoot* root = &roots->roots[i];

    if (root->slot == NULL || *root->slot == NULL) {
      continue;
    }

    if (arena_mark_object(arena, *root->slot)) {
      if (!mark_worklist_push(&worklist, *root->slot)) {
        ok = false;
        break;
      }
    }
  }

  while (ok && worklist.count > 0) {
    GCPtr payload_pointer = mark_worklist_pop(&worklist);
    ok = mark_object_fields_into_worklist(arena, payload_pointer, &worklist);
  }

  mark_worklist_destroy(&worklist);
  return ok;
}

void gc_clear_marks(Arena* arena) {
  for (size_t i = 0; i < arena->page_count; i++) {
    livemap_reset(&arena->pages[i].livemap);
  }
}

bool gc_mark(Arena* arena, const GCRootSet* roots) {
  gc_clear_marks(arena);
  return gc_mark_roots(arena, roots);
}

void gc_sweep(Arena* arena) {
  for (size_t i = 0; i < arena->page_count; i++) {
    Page* page = &arena->pages[i];

    switch (page->state) {
      case GC_PAGE_ACTIVE:
      case GC_PAGE_FULL:
      case GC_PAGE_RELOCATING:
      case GC_PAGE_LARGE:
        if (page->livemap.live_objects == 0) {
          if (arena->active_page == page) {
            arena->active_page = NULL;
          }
          page_reset(page, GC_PAGE_FREE);
        }
        break;
      case GC_PAGE_FREE:
        break;
      default:
        assert(false);
        break;
    }
  }
}

bool gc_collect(Arena* arena, const GCRootSet* roots) {
  if (!gc_mark(arena, roots)) {
    return false;
  }

  gc_sweep(arena);

  if (!gc_evacuate_sparse_pages(arena, roots)) {
    return false;
  }

  if (!gc_verify_relocation(arena)) {
    return false;
  }

  if (!gc_repair_roots(arena, roots)) {
    return false;
  }

  if (!gc_repair_all_objects(arena)) {
    return false;
  }

  gc_finish_relocation(arena);
  gc_sweep(arena);
  return true;
}

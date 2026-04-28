#include <assert.h>
#include <stdlib.h>

#include "gc.h"
#include "livemap.h"
#include "macros.h"
#include "object_header.h"

typedef struct MarkWorklist {
  GCPtr* items;
  size_t count;
  size_t capacity;
} MarkWorklist;

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
  return true;
}

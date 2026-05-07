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

typedef struct RepairVisitState {
  Arena* arena;
  bool ok;
} RepairVisitState;

typedef struct MarkFieldVisitState {
  Arena* arena;
  MarkWorklist* worklist;
} MarkFieldVisitState;

typedef enum GCCollectPhase {
  GC_PHASE_MARK = 0,
  GC_PHASE_SWEEP_DEAD,
  GC_PHASE_EVACUATE,
  GC_PHASE_VERIFY_FORWARDING,
  GC_PHASE_REPAIR_ROOTS,
  GC_PHASE_REPAIR_OBJECTS,
  GC_PHASE_FINISH_RELOCATION,
  GC_PHASE_FINAL_SWEEP,
} GCCollectPhase;

bool gc_verify_relocation(Arena* arena);
bool gc_repair_roots(Arena* arena, const GCRootSet* roots);
bool gc_repair_all_objects(Arena* arena);
void gc_finish_relocation(Arena* arena);

static bool gc_page_state_allows_relocation(const Page* page) {
  return page->state == GC_PAGE_ACTIVE ||
      page->state == GC_PAGE_FULL ||
      page->state == GC_PAGE_RELOCATING ||
      page->state == GC_PAGE_FREE ||
      page->state == GC_PAGE_LARGE;
}

static void gc_assert_phase_invariants(const Arena* arena, GCCollectPhase phase) {
  for (size_t i = 0; i < arena->page_count; i++) {
    const Page* page = &arena->pages[i];

    switch (phase) {
      case GC_PHASE_MARK:
      case GC_PHASE_SWEEP_DEAD:
      case GC_PHASE_EVACUATE:
        assert(gc_page_state_allows_relocation(page));
        break;
      case GC_PHASE_VERIFY_FORWARDING:
      case GC_PHASE_REPAIR_ROOTS:
      case GC_PHASE_REPAIR_OBJECTS:
        assert(gc_page_state_allows_relocation(page));
        if (page->state == GC_PAGE_RELOCATING) {
          assert(page->forwarding_count == 0 || page->forwarding != NULL);
        }
        break;
      case GC_PHASE_FINISH_RELOCATION:
        assert(gc_page_state_allows_relocation(page));
        if (page->state == GC_PAGE_RELOCATING) {
          assert(page->forwarding_count == 0 || page->forwarding != NULL);
        }
        break;
      case GC_PHASE_FINAL_SWEEP:
        assert(page->state != GC_PAGE_RELOCATING);
        break;
      default:
        assert(false);
        break;
    }
  }
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

static bool gc_repair_pointer(Arena* arena, GCPtr* slot) {
  if (slot == NULL || *slot == NULL) {
    return true;
  }

  *slot = gc_forward_if_relocating(arena, *slot);
  return *slot != NULL;
}

static bool gc_repair_field_visitor(
    const ObjectHeader* header,
    void* payload,
    void** field_slot,
    void* user_data) {
  RepairVisitState* state = (RepairVisitState*) user_data;

  (void) header;
  (void) payload;

  return gc_repair_pointer(state->arena, (GCPtr*) field_slot);
}

static bool gc_mark_field_visitor(
    const ObjectHeader* header,
    void* payload,
    void** field_slot,
    void* user_data) {
  MarkFieldVisitState* state = (MarkFieldVisitState*) user_data;
  void* child = *field_slot;

  (void) header;
  (void) payload;

  if (child != NULL && arena_mark_object(state->arena, child)) {
    return mark_worklist_push(state->worklist, child);
  }

  return true;
}

static void gc_repair_visit(Page* page, const ObjectHeader* header, void* payload, void* user_data) {
  RepairVisitState* state = (RepairVisitState*) user_data;
  (void) page;
  (void) header;

  if (!state->ok) {
    return;
  }

  state->ok = arena_visit_object_fields(
      state->arena,
      payload,
      gc_repair_field_visitor,
      state);
}

bool gc_repair_roots(Arena* arena, const GCRootSet* roots) {
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

bool gc_repair_all_objects(Arena* arena) {
  RepairVisitState state = {
    .arena = arena,
    .ok = true,
  };

  arena_for_each_object(arena, gc_repair_visit, &state);
  return state.ok;
}

static bool mark_object_fields_into_worklist(
    Arena* arena,
    void* payload_pointer,
    MarkWorklist* worklist) {
  MarkFieldVisitState state = {
    .arena = arena,
    .worklist = worklist,
  };

  return arena_visit_object_fields(
      arena,
      payload_pointer,
      gc_mark_field_visitor,
      &state);
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

static void gc_promote_surviving_pages(Arena* arena) {
  for (size_t i = 0; i < arena->page_count; i++) {
    Page* page = &arena->pages[i];

    switch (page->state) {
      case GC_PAGE_ACTIVE:
      case GC_PAGE_FULL:
      case GC_PAGE_LARGE:
        if (page->livemap.live_objects == 0) {
          break;
        }

        if (page == arena->active_page && page->state == GC_PAGE_ACTIVE) {
          page->state = GC_PAGE_FULL;
          arena->active_page = NULL;
        }

        page_promote(page);
        break;
      case GC_PAGE_FREE:
      case GC_PAGE_RELOCATING:
        break;
      default:
        assert(false);
        break;
    }
  }
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
          if (page->state == GC_PAGE_RELOCATING) {
            assert(page->forwarding_count == 0 || page->forwarding != NULL);
          }
          page_reset(page, GC_PAGE_FREE, GC_PAGE_AGE_YOUNG);
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
  gc_assert_phase_invariants(arena, GC_PHASE_MARK);
  if (!gc_mark(arena, roots)) {
    return false;
  }

  gc_assert_phase_invariants(arena, GC_PHASE_SWEEP_DEAD);
  gc_sweep(arena);

  gc_assert_phase_invariants(arena, GC_PHASE_EVACUATE);
  if (!gc_evacuate_sparse_pages(arena, roots)) {
    return false;
  }

  gc_assert_phase_invariants(arena, GC_PHASE_VERIFY_FORWARDING);
  if (!gc_verify_relocation(arena)) {
    return false;
  }

  gc_assert_phase_invariants(arena, GC_PHASE_REPAIR_ROOTS);
  if (!gc_repair_roots(arena, roots)) {
    return false;
  }

  gc_assert_phase_invariants(arena, GC_PHASE_REPAIR_OBJECTS);
  if (!gc_repair_all_objects(arena)) {
    return false;
  }

  gc_assert_phase_invariants(arena, GC_PHASE_FINISH_RELOCATION);
  gc_finish_relocation(arena);

  gc_assert_phase_invariants(arena, GC_PHASE_FINAL_SWEEP);
  gc_sweep(arena);
  gc_promote_surviving_pages(arena);
  return true;
}

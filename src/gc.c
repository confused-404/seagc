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

typedef struct RememberedSetVerifyState {
  Arena* arena;
  bool ok;
} RememberedSetVerifyState;

typedef struct MarkFieldVisitState {
  Arena* arena;
  MarkWorklist* worklist;
  bool young_only;
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

static bool gc_test_fail_remembered_grow;
static bool gc_test_fail_root_grow;

bool gc_verify_relocation(Arena* arena);
bool gc_repair_roots(Arena* arena, const GCRootSet* roots);
bool gc_repair_all_objects(Arena* arena);
void gc_finish_relocation(Arena* arena);
bool gc_evacuate_young_pages(Arena* arena, const GCRootSet* roots);

void gc_test_fail_next_remembered_grow(void) {
  gc_test_fail_remembered_grow = true;
}

void gc_test_fail_next_root_grow(void) {
  gc_test_fail_root_grow = true;
}

static bool gc_object_is_young(Arena* arena, const void* object) {
  Page* page;

  if (object == NULL) {
    return false;
  }

  page = arena_find_page(arena, object);
  return page != NULL && page->age == GC_PAGE_AGE_YOUNG;
}

static bool gc_object_is_old(Arena* arena, const void* object) {
  Page* page;

  if (object == NULL) {
    return false;
  }

  page = arena_find_page(arena, object);
  return page != NULL && page->age == GC_PAGE_AGE_OLD;
}

static bool gc_page_can_own_remembered_slots(const Page* page) {
  return page != NULL &&
      page->age == GC_PAGE_AGE_OLD &&
      (page->state == GC_PAGE_ACTIVE ||
          page->state == GC_PAGE_FULL ||
          page->state == GC_PAGE_LARGE);
}

static bool gc_slot_belongs_to_page(const Page* page, const GCPtr* slot) {
  const u8* address = (const u8*) slot;

  return page != NULL &&
      slot != NULL &&
      page->base != NULL &&
      address >= page->base &&
      address < page->top;
}

static void gc_remembered_set_clear(Arena* arena) {
  for (size_t i = 0; i < arena->page_count; i++) {
    arena->pages[i].remembered_set.count = 0;
  }
}

static bool gc_remembered_set_contains(const Arena* arena, GCPtr* slot) {
  const Page* owner_page;
  const RememberedSet* remembered_set;

  owner_page = NULL;
  for (size_t i = 0; i < arena->page_count; i++) {
    const Page* page = &arena->pages[i];

    if (gc_slot_belongs_to_page(page, slot)) {
      owner_page = page;
      break;
    }
  }

  if (owner_page == NULL) {
    return false;
  }

  remembered_set = &owner_page->remembered_set;
  for (size_t i = 0; i < remembered_set->count; i++) {
    if (remembered_set->slots[i] == (void**) slot) {
      return true;
    }
  }

  return false;
}

static bool gc_remember_slot_on_page(Page* owner_page, GCPtr* slot) {
  RememberedSet* remembered_set;
  void*** slots;
  size_t new_capacity;

  if (slot == NULL) {
    return true;
  }

  if (!gc_page_can_own_remembered_slots(owner_page) ||
      !gc_slot_belongs_to_page(owner_page, slot)) {
    return false;
  }

  remembered_set = &owner_page->remembered_set;
  for (size_t i = 0; i < remembered_set->count; i++) {
    if (remembered_set->slots[i] == (void**) slot) {
      return true;
    }
  }

  if (gc_test_fail_remembered_grow && remembered_set->count == remembered_set->capacity) {
    gc_test_fail_remembered_grow = false;
    return false;
  }

  if (remembered_set->count == remembered_set->capacity) {
    if (remembered_set->capacity > SIZE_MAX / 2) {
      return false;
    }

    new_capacity = remembered_set->capacity == 0 ? 8 : remembered_set->capacity * 2;
    if (new_capacity > SIZE_MAX / sizeof(remembered_set->slots[0])) {
      return false;
    }

    slots = (void***) realloc(
        remembered_set->slots,
        new_capacity * sizeof(remembered_set->slots[0]));
    if (slots == NULL) {
      return false;
    }

    remembered_set->slots = slots;
    remembered_set->capacity = new_capacity;
  }

  remembered_set->slots[remembered_set->count++] = (void**) slot;
  return true;
}

size_t gc_remembered_set_count(const Arena* arena) {
  size_t count = 0;

  for (size_t i = 0; i < arena->page_count; i++) {
    count += arena->pages[i].remembered_set.count;
  }

  return count;
}

static bool gc_root_registry_contains(const Arena* arena, GCPtr* slot) {
  const RootRegistry* roots = &arena->roots;

  for (size_t i = 0; i < roots->count; i++) {
    if (roots->slots[i] == (void**) slot) {
      return true;
    }
  }

  return false;
}

bool gc_root_register(Arena* arena, GCPtr* slot) {
  RootRegistry* roots = &arena->roots;
  void*** slots;
  size_t new_capacity;

  if (slot == NULL) {
    return false;
  }

  if (gc_root_registry_contains(arena, slot)) {
    return true;
  }

  if (roots->count == roots->capacity) {
    if (gc_test_fail_root_grow) {
      gc_test_fail_root_grow = false;
      return false;
    }

    if (roots->capacity > SIZE_MAX / 2) {
      return false;
    }

    new_capacity = roots->capacity == 0 ? 16 : roots->capacity * 2;
    if (new_capacity > SIZE_MAX / sizeof(roots->slots[0])) {
      return false;
    }

    slots = (void***) realloc(roots->slots, new_capacity * sizeof(roots->slots[0]));
    if (slots == NULL) {
      return false;
    }

    roots->slots = slots;
    roots->capacity = new_capacity;
  }

  roots->slots[roots->count++] = (void**) slot;
  return true;
}

bool gc_root_unregister(Arena* arena, GCPtr* slot) {
  RootRegistry* roots = &arena->roots;

  if (slot == NULL) {
    return false;
  }

  for (size_t i = 0; i < roots->count; i++) {
    if (roots->slots[i] == (void**) slot) {
      roots->slots[i] = roots->slots[--roots->count];
      return true;
    }
  }

  return false;
}

bool gc_handle_init(Arena* arena, GCHandle* handle, GCPtr value) {
  if (handle == NULL) {
    return false;
  }

  handle->arena = arena;
  handle->slot = value;
  handle->active = false;

  if (arena == NULL) {
    return false;
  }

  if (!gc_root_register(arena, &handle->slot)) {
    handle->arena = NULL;
    handle->slot = NULL;
    return false;
  }

  handle->active = true;
  return true;
}

bool gc_handle_destroy(GCHandle* handle) {
  bool removed;

  if (handle == NULL || !handle->active || handle->arena == NULL) {
    return false;
  }

  removed = gc_root_unregister(handle->arena, &handle->slot);
  handle->arena = NULL;
  handle->slot = NULL;
  handle->active = false;
  return removed;
}

GCPtr gc_handle_get(const GCHandle* handle) {
  if (handle == NULL || !handle->active) {
    return NULL;
  }

  return handle->slot;
}

bool gc_handle_set(GCHandle* handle, GCPtr value) {
  if (handle == NULL || !handle->active) {
    return false;
  }

  handle->slot = value;
  return true;
}

static void gc_prune_remembered_set(Arena* arena) {
  for (size_t page_index = 0; page_index < arena->page_count; page_index++) {
    Page* page = &arena->pages[page_index];
    RememberedSet* remembered_set = &page->remembered_set;
    size_t write = 0;

    if (!gc_page_can_own_remembered_slots(page)) {
      remembered_set->count = 0;
      continue;
    }

    for (size_t read = 0; read < remembered_set->count; read++) {
      void** slot = remembered_set->slots[read];

      if (gc_slot_belongs_to_page(page, (GCPtr*) slot) &&
          slot != NULL &&
          *slot != NULL &&
          gc_object_is_young(arena, *slot)) {
        remembered_set->slots[write++] = slot;
      }
    }

    remembered_set->count = write;
  }
}

static bool gc_verify_remembered_field_visitor(
    const ObjectHeader* header,
    void* payload,
    void** field_slot,
    void* user_data) {
  RememberedSetVerifyState* state = (RememberedSetVerifyState*) user_data;

  (void) header;
  (void) payload;

  if (*field_slot == NULL || !gc_object_is_young(state->arena, *field_slot)) {
    return true;
  }

  return gc_remembered_set_contains(state->arena, (GCPtr*) field_slot);
}

static void gc_verify_remembered_object_visit(
    Page* page,
    const ObjectHeader* header,
    void* payload,
    void* user_data) {
  RememberedSetVerifyState* state = (RememberedSetVerifyState*) user_data;

  (void) header;

  if (!state->ok || page->state == GC_PAGE_FREE || page->state == GC_PAGE_RELOCATING) {
    return;
  }

  if (page->age != GC_PAGE_AGE_OLD) {
    return;
  }

  state->ok = arena_visit_object_fields(
      state->arena,
      payload,
      gc_verify_remembered_field_visitor,
      state);
}

bool gc_verify_remembered_set(Arena* arena) {
  RememberedSetVerifyState state = {
    .arena = arena,
    .ok = true,
  };

  arena_for_each_object(arena, gc_verify_remembered_object_visit, &state);
  return state.ok;
}

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
  ArenaCollectionTrigger trigger = arena_collection_trigger(arena);
  void* payload;

  if (trigger == GC_TRIGGER_FULL) {
    if (!gc_collect(arena, roots)) {
      return NULL;
    }
  } else if (trigger == GC_TRIGGER_YOUNG) {
    if (!gc_collect_young(arena, roots)) {
      return NULL;
    }
  }

  payload = arena_alloc_traced(arena, payload_size, trace);

  if (payload != NULL) {
    return payload;
  }

  if (gc_collect_young(arena, roots)) {
    payload = arena_alloc_traced(arena, payload_size, trace);
    if (payload != NULL) {
      return payload;
    }
  }

  if (!gc_collect(arena, roots)) {
    return NULL;
  }

  return arena_alloc_traced(arena, payload_size, trace);
}

bool gc_store_pointer(Arena* arena, void* owner, GCPtr* slot, GCPtr value) {
  Page* owner_page;

  assert(slot != NULL);

  owner_page = arena_find_page(arena, owner);
  if (gc_page_can_own_remembered_slots(owner_page) && gc_object_is_young(arena, value)) {
    if (!gc_remember_slot_on_page(owner_page, slot)) {
      return false;
    }
  }

  *slot = value;
  return true;
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

  *slot = gc_forward_existing_if_relocating(arena, *slot);
  return *slot != NULL;
}

static bool gc_repair_field_visitor(
    const ObjectHeader* header,
    void* payload,
    void** field_slot,
    void* user_data) {
  RepairVisitState* state = (RepairVisitState*) user_data;

  (void) header;

  if (!gc_repair_pointer(state->arena, (GCPtr*) field_slot)) {
    return false;
  }

  if (gc_object_is_old(state->arena, payload) && gc_object_is_young(state->arena, *field_slot)) {
    Page* owner_page = arena_find_page(state->arena, payload);

    return gc_remember_slot_on_page(owner_page, (GCPtr*) field_slot);
  }

  return true;
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

  if (state->young_only && !gc_object_is_young(state->arena, child)) {
    return true;
  }

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

  if (page->state == GC_PAGE_RELOCATING || page->state == GC_PAGE_FREE) {
    return;
  }

  state->ok = arena_visit_object_fields(
      state->arena,
      payload,
      gc_repair_field_visitor,
      state);
}

bool gc_repair_roots(Arena* arena, const GCRootSet* roots) {
  if (roots != NULL) {
    for (size_t i = 0; i < roots->count; i++) {
      const GCRoot* root = &roots->roots[i];
      if (!gc_repair_pointer(arena, root->slot)) {
        return false;
      }
    }
  }

  for (size_t i = 0; i < arena->roots.count; i++) {
    if (!gc_repair_pointer(arena, (GCPtr*) arena->roots.slots[i])) {
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
    .young_only = false,
  };

  return arena_visit_object_fields(
      arena,
      payload_pointer,
      gc_mark_field_visitor,
      &state);
}

static bool gc_mark_root_slot(Arena* arena, GCPtr* slot, MarkWorklist* worklist) {
  if (slot == NULL || *slot == NULL) {
    return true;
  }

  if (arena_mark_object(arena, *slot)) {
    return mark_worklist_push(worklist, *slot);
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

  if (roots != NULL) {
    for (size_t i = 0; i < roots->count; i++) {
      const GCRoot* root = &roots->roots[i];

      if (!gc_mark_root_slot(arena, root->slot, &worklist)) {
        ok = false;
        break;
      }
    }
  }

  for (size_t i = 0; ok && i < arena->roots.count; i++) {
    ok = gc_mark_root_slot(arena, (GCPtr*) arena->roots.slots[i], &worklist);
  }

  while (ok && worklist.count > 0) {
    GCPtr payload_pointer = mark_worklist_pop(&worklist);
    ok = mark_object_fields_into_worklist(arena, payload_pointer, &worklist);
  }

  mark_worklist_destroy(&worklist);
  return ok;
}

static bool mark_young_object_fields_into_worklist(
    Arena* arena,
    void* payload_pointer,
    MarkWorklist* worklist) {
  MarkFieldVisitState state = {
    .arena = arena,
    .worklist = worklist,
    .young_only = true,
  };

  return arena_visit_object_fields(
      arena,
      payload_pointer,
      gc_mark_field_visitor,
      &state);
}

static bool gc_mark_young_root_slot(Arena* arena, GCPtr* slot, MarkWorklist* worklist) {
  if (slot == NULL || *slot == NULL || !gc_object_is_young(arena, *slot)) {
    return true;
  }

  if (arena_mark_object(arena, *slot)) {
    return mark_worklist_push(worklist, *slot);
  }

  return true;
}

static void gc_clear_young_marks(Arena* arena) {
  for (size_t i = 0; i < arena->page_count; i++) {
    if (arena->pages[i].age == GC_PAGE_AGE_YOUNG) {
      livemap_reset(&arena->pages[i].livemap);
    }
  }
}

static bool gc_mark_young_roots(Arena* arena, const GCRootSet* roots) {
  MarkWorklist worklist = {
    .items = NULL,
    .count = 0,
    .capacity = 0,
  };
  bool ok = true;

  if (roots != NULL) {
    for (size_t i = 0; i < roots->count; i++) {
      const GCRoot* root = &roots->roots[i];

      if (!gc_mark_young_root_slot(arena, root->slot, &worklist)) {
        ok = false;
        break;
      }
    }
  }

  for (size_t i = 0; ok && i < arena->roots.count; i++) {
    ok = gc_mark_young_root_slot(arena, (GCPtr*) arena->roots.slots[i], &worklist);
  }

  for (size_t page_index = 0; ok && page_index < arena->page_count; page_index++) {
    RememberedSet* remembered_set = &arena->pages[page_index].remembered_set;

    for (size_t i = 0; ok && i < remembered_set->count; i++) {
      GCPtr* slot = (GCPtr*) remembered_set->slots[i];

      if (slot == NULL || *slot == NULL || !gc_object_is_young(arena, *slot)) {
        continue;
      }

      if (arena_mark_object(arena, *slot)) {
        ok = mark_worklist_push(&worklist, *slot);
      }
    }
  }

  while (ok && worklist.count > 0) {
    GCPtr payload_pointer = mark_worklist_pop(&worklist);
    ok = mark_young_object_fields_into_worklist(arena, payload_pointer, &worklist);
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

static void gc_clear_active_page_reference(Arena* arena, Page* page) {
  if (arena->nursery_active_page == page) {
    arena->nursery_active_page = NULL;
  }
  if (arena->survivor_active_page == page) {
    arena->survivor_active_page = NULL;
  }
  if (arena->old_active_page == page) {
    arena->old_active_page = NULL;
  }
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

        if (page->state == GC_PAGE_ACTIVE) {
          page->state = GC_PAGE_FULL;
          gc_clear_active_page_reference(arena, page);
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
          gc_clear_active_page_reference(arena, page);
          if (page->state == GC_PAGE_RELOCATING) {
            assert(page->forwarding_count == 0 || page->forwarding != NULL);
          }
          page_reset(page, GC_PAGE_FREE, GC_PAGE_AGE_YOUNG, GC_SPACE_NURSERY);
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

static void gc_sweep_dead_young(Arena* arena) {
  for (size_t i = 0; i < arena->page_count; i++) {
    Page* page = &arena->pages[i];

    if (page->age != GC_PAGE_AGE_YOUNG) {
      continue;
    }

    switch (page->state) {
      case GC_PAGE_ACTIVE:
      case GC_PAGE_FULL:
      case GC_PAGE_LARGE:
        if (page->livemap.live_objects == 0) {
          gc_clear_active_page_reference(arena, page);
          page_reset(page, GC_PAGE_FREE, GC_PAGE_AGE_YOUNG, GC_SPACE_NURSERY);
        }
        break;
      case GC_PAGE_FREE:
        break;
      case GC_PAGE_RELOCATING:
      default:
        assert(false);
        break;
    }
  }
}

bool gc_collect_young(Arena* arena, const GCRootSet* roots) {
  if (!gc_verify_remembered_set(arena)) {
    assert(false);
    return false;
  }

  gc_clear_young_marks(arena);
  if (!gc_mark_young_roots(arena, roots)) {
    return false;
  }

  gc_sweep_dead_young(arena);
  if (!gc_evacuate_young_pages(arena, roots)) {
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
  gc_prune_remembered_set(arena);
  if (!gc_verify_remembered_set(arena)) {
    assert(false);
    return false;
  }

  return true;
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
  gc_remembered_set_clear(arena);
  if (!gc_verify_remembered_set(arena)) {
    assert(false);
    return false;
  }
  return true;
}

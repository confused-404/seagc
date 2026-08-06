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

typedef struct RememberedSetRebuildState {
  Arena* arena;
  bool ok;
} RememberedSetRebuildState;

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
static bool gc_collect_young_with_reason(
    Arena* arena,
    const GCRootSet* roots,
    GCCollectionReason reason);
static bool gc_collect_with_reason(
    Arena* arena,
    const GCRootSet* roots,
    GCCollectionReason reason);

/* Make the next required remembered-set growth fail for testing. */
void gc_test_fail_next_remembered_grow(void) {
  gc_test_fail_remembered_grow = true;
}

/* Make the next required root-registry growth fail for testing. */
void gc_test_fail_next_root_grow(void) {
  gc_test_fail_root_grow = true;
}

/* Report whether a payload belongs to a live young page. */
static bool gc_object_is_young(Arena* arena, const void* object) {
  Page* page;

  if (object == NULL) {
    return false;
  }

  page = arena_find_page(arena, object);
  return page != NULL && page->age == GC_PAGE_AGE_YOUNG;
}

/* Report whether a payload belongs to a live old page. */
static bool gc_object_is_old(Arena* arena, const void* object) {
  Page* page;

  if (object == NULL) {
    return false;
  }

  page = arena_find_page(arena, object);
  return page != NULL && page->age == GC_PAGE_AGE_OLD;
}

/* Check whether a page may legally contain remembered pointer slots. */
static bool gc_page_can_own_remembered_slots(const Page* page) {
  return page != NULL &&
      page->age == GC_PAGE_AGE_OLD &&
      (page->state == GC_PAGE_ACTIVE ||
          page->state == GC_PAGE_FULL ||
          page->state == GC_PAGE_LARGE);
}

/* Check whether a slot address lies in a page's allocated range. */
static bool gc_slot_belongs_to_page(const Page* page, const GCPtr* slot) {
  const u8* address = (const u8*) slot;

  return page != NULL &&
      slot != NULL &&
      page->base != NULL &&
      address >= page->base &&
      address < page->top;
}

/* Empty every page-local remembered set without freeing capacity. */
static void gc_remembered_set_clear(Arena* arena) {
  for (size_t i = 0; i < arena->page_count; i++) {
    arena->pages[i].remembered_set.count = 0;
  }
}

/* Sum committed bytes on non-free pages in one allocation space. */
static size_t gc_space_used_bytes(const Arena* arena, PageSpace space) {
  size_t bytes = 0;

  for (size_t i = 0; i < arena->page_count; i++) {
    const Page* page = &arena->pages[i];

    if (page->state != GC_PAGE_FREE && page->space == space) {
      bytes += page->used;
    }
  }

  return bytes;
}

/* Sum committed bytes on all non-free young pages. */
static size_t gc_young_used_bytes(const Arena* arena) {
  size_t bytes = 0;

  for (size_t i = 0; i < arena->page_count; i++) {
    const Page* page = &arena->pages[i];

    if (page->state != GC_PAGE_FREE && page->age == GC_PAGE_AGE_YOUNG) {
      bytes += page->used;
    }
  }

  return bytes;
}

/* Sum committed bytes across the entire non-free heap. */
static size_t gc_total_used_bytes(const Arena* arena) {
  size_t bytes = 0;

  for (size_t i = 0; i < arena->page_count; i++) {
    const Page* page = &arena->pages[i];

    if (page->state != GC_PAGE_FREE) {
      bytes += page->used;
    }
  }

  return bytes;
}

/* Add the collection's net byte reduction to reclaimed statistics. */
static void gc_record_reclaimed(Arena* arena, size_t before_bytes) {
  const size_t after_bytes = gc_total_used_bytes(arena);

  if (before_bytes > after_bytes) {
    arena->stats.reclaimed_bytes += before_bytes - after_bytes;
  }
}

/* Adapt nursery size and promotion age from young-collection outcomes. */
static void gc_policy_after_young(Arena* arena, size_t young_before, size_t promoted_before) {
  const size_t young_after = gc_young_used_bytes(arena);
  const size_t survivor_after = gc_space_used_bytes(arena, GC_SPACE_SURVIVOR);
  const size_t promoted_delta = arena->stats.promoted_bytes - promoted_before;
  ArenaGCPolicy* policy = &arena->policy;

  if (young_before == 0) {
    return;
  }

  if (promoted_delta > 0 && policy->promotion_age > 1u) {
    policy->promotion_age--;
  } else if (young_after * 4u < young_before &&
      policy->nursery_page_target < policy->max_nursery_pages) {
    policy->nursery_page_target++;
  } else if ((young_after * 2u > young_before || survivor_after * 2u > young_before) &&
      policy->nursery_page_target > 1u) {
    policy->nursery_page_target--;
  }
}

/* Check whether a slot appears in its owning page's remembered set. */
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

/* Add a valid slot to an old page's remembered set exactly once. */
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

/* Count remembered slots across all pages. */
size_t gc_remembered_set_count(const Arena* arena) {
  size_t count = 0;

  for (size_t i = 0; i < arena->page_count; i++) {
    count += arena->pages[i].remembered_set.count;
  }

  return count;
}

/* Check whether a root slot address is already registered. */
static bool gc_root_registry_contains(const Arena* arena, GCPtr* slot) {
  const RootRegistry* roots = &arena->roots;

  for (size_t i = 0; i < roots->count; i++) {
    if (roots->slots[i] == (void**) slot) {
      return true;
    }
  }

  return false;
}

/* Register a persistent writable root slot, deduplicating its address. */
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

/* Remove a persistent root slot by address. */
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

/* Initialize a handle and register its internal slot as a root. */
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

/* Unregister and invalidate an active handle. */
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

/* Return an active handle's current payload pointer. */
GCPtr gc_handle_get(const GCHandle* handle) {
  if (handle == NULL || !handle->active) {
    return NULL;
  }

  return handle->slot;
}

/* Replace the rooted value held by an active handle. */
bool gc_handle_set(GCHandle* handle, GCPtr value) {
  if (handle == NULL || !handle->active) {
    return false;
  }

  handle->slot = value;
  return true;
}

/* Remove stale, invalid, and no-longer-young remembered entries. */
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

/* Require each old-to-young field to have a remembered entry. */
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

/* Verify remembered-set coverage for one old object's fields. */
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

/* Verify that every old-to-young field has a remembered entry. */
bool gc_verify_remembered_set(Arena* arena) {
  RememberedSetVerifyState state = {
    .arena = arena,
    .ok = true,
  };

  arena_for_each_object(arena, gc_verify_remembered_object_visit, &state);
  return state.ok;
}

/* Record one old-object field when it points to a young object. */
static bool gc_rebuild_remembered_field_visitor(
    const ObjectHeader* header,
    void* payload,
    void** field_slot,
    void* user_data) {
  RememberedSetRebuildState* state = (RememberedSetRebuildState*) user_data;
  Page* owner_page;

  (void) header;

  if (*field_slot == NULL || !gc_object_is_young(state->arena, *field_slot)) {
    return true;
  }

  owner_page = arena_find_page(state->arena, payload);
  return gc_remember_slot_on_page(owner_page, (GCPtr*) field_slot);
}

/* Rebuild remembered entries for one eligible old object. */
static void gc_rebuild_remembered_object_visit(
    Page* page,
    const ObjectHeader* header,
    void* payload,
    void* user_data) {
  RememberedSetRebuildState* state = (RememberedSetRebuildState*) user_data;

  (void) header;

  if (!state->ok ||
      page->state == GC_PAGE_FREE ||
      page->state == GC_PAGE_RELOCATING ||
      page->age != GC_PAGE_AGE_OLD) {
    return;
  }

  state->ok = arena_visit_object_fields(
      state->arena,
      payload,
      gc_rebuild_remembered_field_visitor,
      state);
}

/* Recreate all remembered sets by scanning old traced objects. */
static bool gc_rebuild_remembered_sets(Arena* arena) {
  RememberedSetRebuildState state = {
    .arena = arena,
    .ok = true,
  };

  gc_remembered_set_clear(arena);
  arena_for_each_object(arena, gc_rebuild_remembered_object_visit, &state);
  return state.ok;
}

/* Check that a page state is recognized by collection phase assertions. */
static bool gc_page_state_allows_relocation(const Page* page) {
  return page->state == GC_PAGE_ACTIVE ||
      page->state == GC_PAGE_FULL ||
      page->state == GC_PAGE_RELOCATING ||
      page->state == GC_PAGE_FREE ||
      page->state == GC_PAGE_LARGE;
}

/* Assert page-state invariants expected at a full-collection phase boundary. */
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

/* Allocate a pointer-free nursery object with GC pressure handling. */
void* gc_alloc(Arena* arena, size_t payload_size, const GCRootSet* roots) {
  return gc_alloc_traced(arena, payload_size, NULL, roots);
}

/* Allocate a traced nursery object, collecting and retrying as needed. */
void* gc_alloc_traced(
    Arena* arena,
    size_t payload_size,
    const TraceDescriptor* trace,
    const GCRootSet* roots) {
  ArenaCollectionTrigger trigger = arena_collection_trigger(arena);
  void* payload;

  if (trigger == GC_TRIGGER_FULL) {
    if (!gc_collect_with_reason(arena, roots, GC_REASON_OLD_SPACE_PRESSURE)) {
      return NULL;
    }
  } else if (trigger == GC_TRIGGER_YOUNG) {
    if (!gc_collect_young_with_reason(
        arena,
        roots,
        GC_REASON_ALLOCATION_NURSERY_PRESSURE)) {
      return NULL;
    }
  }

  payload = arena_alloc_traced(arena, payload_size, trace);

  if (payload != NULL) {
    return payload;
  }

  if (gc_collect_young_with_reason(arena, roots, GC_REASON_ALLOCATION_FAILURE)) {
    payload = arena_alloc_traced(arena, payload_size, trace);
    if (payload != NULL) {
      return payload;
    }
  }

  if (!gc_collect_with_reason(arena, roots, GC_REASON_ALLOCATION_FAILURE)) {
    return NULL;
  }

  return arena_alloc_traced(arena, payload_size, trace);
}

/* Allocate a pointer-free old object with full-collection retry. */
void* gc_alloc_old(Arena* arena, size_t payload_size, const GCRootSet* roots) {
  return gc_alloc_old_traced(arena, payload_size, NULL, roots);
}

/* Allocate a traced old object with full-collection retry. */
void* gc_alloc_old_traced(
    Arena* arena,
    size_t payload_size,
    const TraceDescriptor* trace,
    const GCRootSet* roots) {
  void* payload;

  if (arena_collection_trigger(arena) == GC_TRIGGER_FULL) {
    if (!gc_collect_with_reason(arena, roots, GC_REASON_OLD_SPACE_PRESSURE)) {
      return NULL;
    }
  }

  payload = arena_alloc_traced_in_space(arena, payload_size, trace, GC_SPACE_OLD);
  if (payload != NULL) {
    return payload;
  }

  if (!gc_collect_with_reason(arena, roots, GC_REASON_ALLOCATION_FAILURE)) {
    return NULL;
  }

  return arena_alloc_traced_in_space(arena, payload_size, trace, GC_SPACE_OLD);
}

/* Apply the old-to-young write barrier before publishing a slot value. */
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

/* Remap a relocating slot in place, forwarding on demand if necessary. */
GCPtr gc_load_pointer(Arena* arena, GCPtr* slot) {
  Page* value_page;
  GCPtr value;
  GCPtr forwarded;

  if (slot == NULL || *slot == NULL) {
    return NULL;
  }

  value = *slot;
  forwarded = gc_forward_if_relocating(arena, value);
  if (forwarded != NULL) {
    *slot = forwarded;
    return forwarded;
  }

  value_page = arena_find_page(arena, value);
  if (value_page != NULL && value_page->state == GC_PAGE_RELOCATING) {
    return NULL;
  }

  return value;
}

/* Release a mark worklist and return it to an empty state. */
static void mark_worklist_destroy(MarkWorklist* worklist) {
  free(worklist->items);
  worklist->items = NULL;
  worklist->count = 0;
  worklist->capacity = 0;
}

/* Append a payload pointer, growing the mark worklist safely. */
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

/* Pop the most recently queued payload from the mark worklist. */
static GCPtr mark_worklist_pop(MarkWorklist* worklist) {
  assert(worklist->count > 0);
  return worklist->items[--worklist->count];
}

/* Replace a source pointer with its existing forwarded destination. */
static bool gc_repair_pointer(Arena* arena, GCPtr* slot) {
  if (slot == NULL || *slot == NULL) {
    return true;
  }

  *slot = gc_forward_existing_if_relocating(arena, *slot);
  return *slot != NULL;
}

/* Repair one traced field and remember any resulting old-to-young edge. */
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

/* Mark an eligible child and enqueue it for transitive tracing. */
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

/* Repair all traced fields in one non-source object. */
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

/* Repair transient and registered root slots after relocation. */
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

/* Repair traced fields in every non-free, non-source object. */
bool gc_repair_all_objects(Arena* arena) {
  RepairVisitState state = {
    .arena = arena,
    .ok = true,
  };

  arena_for_each_object(arena, gc_repair_visit, &state);
  return state.ok;
}

/* Repair fields inside destination copies produced by young evacuation. */
static bool gc_repair_relocated_young_objects(Arena* arena) {
  RepairVisitState state = {
    .arena = arena,
    .ok = true,
  };

  for (size_t i = 0; state.ok && i < arena->page_count; i++) {
    Page* source_page = &arena->pages[i];

    if (source_page->state != GC_PAGE_RELOCATING ||
        source_page->age != GC_PAGE_AGE_YOUNG) {
      continue;
    }

    for (size_t j = 0; state.ok && j < source_page->forwarding_count; j++) {
      void* payload = source_page->forwarding[j].new_payload;

      state.ok = arena_visit_object_fields(
          arena,
          payload,
          gc_repair_field_visitor,
          &state);
    }
  }

  return state.ok;
}

/* Repair all still-valid slots recorded on old pages. */
static bool gc_repair_remembered_slots(Arena* arena) {
  for (size_t page_index = 0; page_index < arena->page_count; page_index++) {
    Page* page = &arena->pages[page_index];
    RememberedSet* remembered_set = &page->remembered_set;

    if (!gc_page_can_own_remembered_slots(page)) {
      continue;
    }

    for (size_t i = 0; i < remembered_set->count; i++) {
      GCPtr* slot = (GCPtr*) remembered_set->slots[i];

      if (!gc_slot_belongs_to_page(page, slot)) {
        continue;
      }

      if (!gc_repair_pointer(arena, slot)) {
        return false;
      }
    }
  }

  return true;
}

/* Trace all child fields of one marked object into the full mark worklist. */
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

/* Mark and enqueue the object currently held by one root slot. */
static bool gc_mark_root_slot(Arena* arena, GCPtr* slot, MarkWorklist* worklist) {
  if (slot == NULL || *slot == NULL) {
    return true;
  }

  if (arena_mark_object(arena, *slot)) {
    return mark_worklist_push(worklist, *slot);
  }

  return true;
}

/* Mark the full transitive closure of transient and registered roots. */
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

/* Trace only young children of one marked young object. */
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

/* Mark and enqueue a root value only when it is young. */
static bool gc_mark_young_root_slot(Arena* arena, GCPtr* slot, MarkWorklist* worklist) {
  if (slot == NULL || *slot == NULL || !gc_object_is_young(arena, *slot)) {
    return true;
  }

  if (arena_mark_object(arena, *slot)) {
    return mark_worklist_push(worklist, *slot);
  }

  return true;
}

/* Clear liveness information only on young pages. */
static void gc_clear_young_marks(Arena* arena) {
  for (size_t i = 0; i < arena->page_count; i++) {
    if (arena->pages[i].age == GC_PAGE_AGE_YOUNG) {
      livemap_reset(&arena->pages[i].livemap);
    }
  }
}

/* Mark young reachability from roots and remembered old slots. */
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

/* Clear liveness information on every page. */
void gc_clear_marks(Arena* arena) {
  for (size_t i = 0; i < arena->page_count; i++) {
    livemap_reset(&arena->pages[i].livemap);
  }
}

/* Reset marks and compute full-heap reachability from all roots. */
bool gc_mark(Arena* arena, const GCRootSet* roots) {
  gc_clear_marks(arena);
  return gc_mark_roots(arena, roots);
}

/* Remove a page from any active allocation cursor that references it. */
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

/* Promote every marked surviving page after a full collection. */
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

/* Reset pages with no marked objects to reusable free pages. */
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

/* Reset only unmarked young pages before young evacuation. */
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

/* Run a complete minor collection and record its trigger reason. */
static bool gc_collect_young_with_reason(
    Arena* arena,
    const GCRootSet* roots,
    GCCollectionReason reason) {
  const size_t before_bytes = gc_total_used_bytes(arena);
  const size_t young_before = gc_young_used_bytes(arena);
  const size_t promoted_before = arena->stats.promoted_bytes;

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

  if (!gc_repair_relocated_young_objects(arena)) {
    return false;
  }

  if (!gc_repair_remembered_slots(arena)) {
    return false;
  }

  gc_finish_relocation(arena);
  gc_prune_remembered_set(arena);
  if (!gc_verify_remembered_set(arena)) {
    assert(false);
    return false;
  }

  arena->stats.minor_collections++;
  arena->stats.last_collection_reason = reason;
  gc_record_reclaimed(arena, before_bytes);
  gc_policy_after_young(arena, young_before, promoted_before);
  arena_stats_recompute_live(arena);
  return true;
}

/* Run an explicitly requested young-generation collection. */
bool gc_collect_young(Arena* arena, const GCRootSet* roots) {
  return gc_collect_young_with_reason(arena, roots, GC_REASON_EXPLICIT_YOUNG);
}

/* Run every full-collection phase and record its trigger reason. */
static bool gc_collect_with_reason(
    Arena* arena,
    const GCRootSet* roots,
    GCCollectionReason reason) {
  const size_t before_bytes = gc_total_used_bytes(arena);

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
  if (!gc_rebuild_remembered_sets(arena)) {
    return false;
  }
  if (!gc_verify_remembered_set(arena)) {
    assert(false);
    return false;
  }
  arena->stats.full_collections++;
  arena->stats.last_collection_reason = reason;
  gc_record_reclaimed(arena, before_bytes);
  arena_stats_recompute_live(arena);
  return true;
}

/* Run an explicitly requested full-heap collection. */
bool gc_collect(Arena* arena, const GCRootSet* roots) {
  return gc_collect_with_reason(arena, roots, GC_REASON_EXPLICIT_FULL);
}

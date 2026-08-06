#ifndef GC_H
#define GC_H

#include "arena.h"
#include "types.h"

typedef void* GCPtr;

typedef struct GCRoot {
  GCPtr* slot;
} GCRoot;

typedef struct GCRootSet {
  const GCRoot* roots;
  size_t count;
} GCRootSet;

typedef struct GCHandle {
  Arena* arena;
  GCPtr slot;
  bool active;
} GCHandle;

#define GC_STORE(arena, owner, field, value) \
  gc_store_pointer((arena), (owner), (GCPtr*) &(owner)->field, (GCPtr) (value))
/* Store through an arbitrary pointer slot inside a heap object. */
#define GC_STORE_SLOT(arena, owner, slot, value) \
  gc_store_pointer((arena), (owner), (GCPtr*) (slot), (GCPtr) (value))
/* Load and repair a slot that may still point at a relocating object. */
#define GC_LOAD_SLOT(arena, slot) \
  gc_load_pointer((arena), (GCPtr*) (slot))

/* Allocate a pointer-free nursery object with collection and retry. */
void* gc_alloc(Arena* arena, size_t payload_size, const GCRootSet* roots);
/* Allocate an exactly traced nursery object with collection and retry. */
void* gc_alloc_traced(
    Arena* arena,
    size_t payload_size,
    const TraceDescriptor* trace,
    const GCRootSet* roots);
/* Allocate directly in the old generation; retry after full collection if needed. */
void* gc_alloc_old(Arena* arena, size_t payload_size, const GCRootSet* roots);
/* Old-generation allocation with exact pointer-field metadata. */
void* gc_alloc_old_traced(
    Arena* arena,
    size_t payload_size,
    const TraceDescriptor* trace,
    const GCRootSet* roots);
/* Apply the old-to-young barrier before publishing a heap pointer. */
bool gc_store_pointer(Arena* arena, void* owner, GCPtr* slot, GCPtr value);
/* Return NULL only for a NULL slot/value or a failed forwarding repair. */
GCPtr gc_load_pointer(Arena* arena, GCPtr* slot);
/* Clear liveness information on every page. */
void gc_clear_marks(Arena* arena);
/* Reset marks and compute full reachability from all roots. */
bool gc_mark(Arena* arena, const GCRootSet* roots);
/* Mark the transitive closure of transient and registered roots. */
bool gc_mark_roots(Arena* arena, const GCRootSet* roots);
/* Reset every page containing no marked objects. */
void gc_sweep(Arena* arena);
/* Count remembered slots across all pages. */
size_t gc_remembered_set_count(const Arena* arena);
/* Verify that every old-to-young field is remembered. */
bool gc_verify_remembered_set(Arena* arena);
/* Run an explicitly requested young-generation collection. */
bool gc_collect_young(Arena* arena, const GCRootSet* roots);
/* Run an explicitly requested full-heap collection. */
bool gc_collect(Arena* arena, const GCRootSet* roots);
/* Register a persistent writable root slot by address. */
bool gc_root_register(Arena* arena, GCPtr* slot);
/* Unregister a persistent root slot by address. */
bool gc_root_unregister(Arena* arena, GCPtr* slot);
/* Initialize a handle and register its internal root slot. */
bool gc_handle_init(Arena* arena, GCHandle* handle, GCPtr value);
/* Unregister and invalidate an active handle. */
bool gc_handle_destroy(GCHandle* handle);
/* Return an active handle's current payload pointer. */
GCPtr gc_handle_get(const GCHandle* handle);
/* Replace the rooted value held by an active handle. */
bool gc_handle_set(GCHandle* handle, GCPtr value);
/* Return or create a relocation destination for a payload. */
void* gc_forward_if_relocating(Arena* arena, void* object);
/* Return only an already-recorded relocation destination. */
void* gc_forward_existing_if_relocating(Arena* arena, void* object);
/* Transactionally evacuate sparse normal pages. */
bool gc_evacuate_sparse_pages(Arena* arena, const GCRootSet* roots);
/* Transactionally evacuate, age, and promote live young objects. */
bool gc_evacuate_young_pages(Arena* arena, const GCRootSet* roots);
/* Release relocation source pages after all roots and object fields are repaired. */
void gc_finish_relocation(Arena* arena);
/* Make the next required remembered-set growth fail for testing. */
void gc_test_fail_next_remembered_grow(void);
/* Make the next required root-registry growth fail for testing. */
void gc_test_fail_next_root_grow(void);
/* Fail forwarding after a chosen number of entries for testing. */
void gc_test_fail_forwarding_after(size_t successful_forwarding_entries);

#endif

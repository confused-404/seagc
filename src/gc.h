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

void* gc_alloc(Arena* arena, size_t payload_size, const GCRootSet* roots);
void* gc_alloc_traced(
    Arena* arena,
    size_t payload_size,
    const TraceDescriptor* trace,
    const GCRootSet* roots);
bool gc_store_pointer(Arena* arena, void* owner, GCPtr* slot, GCPtr value);
void gc_clear_marks(Arena* arena);
bool gc_mark(Arena* arena, const GCRootSet* roots);
bool gc_mark_roots(Arena* arena, const GCRootSet* roots);
void gc_sweep(Arena* arena);
bool gc_verify_remembered_set(Arena* arena);
bool gc_collect_young(Arena* arena, const GCRootSet* roots);
bool gc_collect(Arena* arena, const GCRootSet* roots);
bool gc_root_register(Arena* arena, GCPtr* slot);
bool gc_root_unregister(Arena* arena, GCPtr* slot);
bool gc_handle_init(Arena* arena, GCHandle* handle, GCPtr value);
bool gc_handle_destroy(GCHandle* handle);
GCPtr gc_handle_get(const GCHandle* handle);
bool gc_handle_set(GCHandle* handle, GCPtr value);
void* gc_forward_if_relocating(Arena* arena, void* object);
void* gc_forward_existing_if_relocating(Arena* arena, void* object);
bool gc_evacuate_sparse_pages(Arena* arena, const GCRootSet* roots);
bool gc_evacuate_young_pages(Arena* arena, const GCRootSet* roots);
void gc_test_fail_next_remembered_grow(void);
void gc_test_fail_next_root_grow(void);
void gc_test_fail_forwarding_after(size_t successful_forwarding_entries);

#endif

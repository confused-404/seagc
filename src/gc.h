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
bool gc_collect_young(Arena* arena, const GCRootSet* roots);
bool gc_collect(Arena* arena, const GCRootSet* roots);
void* gc_forward_if_relocating(Arena* arena, void* object);
bool gc_evacuate_sparse_pages(Arena* arena, const GCRootSet* roots);

#endif

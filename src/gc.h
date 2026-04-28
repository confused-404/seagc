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

void gc_clear_marks(Arena* arena);
bool gc_mark(Arena* arena, const GCRootSet* roots);
bool gc_mark_roots(Arena* arena, const GCRootSet* roots);
void gc_sweep(Arena* arena);

#endif

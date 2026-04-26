#ifndef TRACE_DESCRIPTOR_H
#define TRACE_DESCRIPTOR_H

#include "types.h"

typedef struct TraceDescriptor {
  size_t pointer_count;
  const size_t* pointer_offsets;
} TraceDescriptor;

#endif

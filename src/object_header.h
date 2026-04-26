#ifndef OBJECT_HEADER_H
#define OBJECT_HEADER_H

#include "types.h"
#include "trace_descriptor.h"

typedef struct ObjectHeader {
  size_t size;
  const TraceDescriptor* trace;
} ObjectHeader;

#endif

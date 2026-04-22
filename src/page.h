#ifndef PAGE_H
#define PAGE_H

#include "types.h"

typedef enum PageState {
  GC_PAGE_FREE = 0,
  GC_PAGE_ACTIVE,
  GC_PAGE_FULL,
  GC_PAGE_LARGE,
} PageState;

typedef struct Page {
  u8* base;
  u8* top;
  u8* limit;
  size_t used;
  size_t capacity;
  PageState state;
} Page;

#endif

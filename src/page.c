#include <stdlib.h>

#include "page.h"
#include "livemap.h"

void page_init(Page *page, u8* base, size_t capacity, PageState state) {
  page->base = base;
  page->capacity = capacity;
  page_reset(page, state);
}

void page_reset(Page *page, PageState state) {
  page->top = page->base;
  page->limit = page->base + page->capacity;
  page->used = 0;
  page->state = state;
  livemap_reset(&page->livemap);
}

void page_release(Page* page) {
  free(page->base);
  page->base = NULL;
  page->top = NULL;
  page->limit = NULL;
  page->used = 0;
  page->capacity = 0;
  page->state = GC_PAGE_FREE;
  livemap_reset(&page->livemap);
}

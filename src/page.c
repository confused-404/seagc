#include <stdlib.h>

#include "page.h"
#include "livemap.h"

/* Release all forwarding entries owned by a page. */
void page_clear_forwarding(Page* page) {
  free(page->forwarding);
  page->forwarding = NULL;
  page->forwarding_count = 0;
  page->forwarding_capacity = 0;
}

/* Empty a remembered set while retaining its allocated storage. */
static void page_clear_remembered_set(Page* page) {
  page->remembered_set.count = 0;
}

/* Release all remembered-set storage owned by a page. */
static void page_release_remembered_set(Page* page) {
  free(page->remembered_set.slots);
  page->remembered_set.slots = NULL;
  page->remembered_set.count = 0;
  page->remembered_set.capacity = 0;
}

/* Attach backing memory and initialize a new page descriptor. */
void page_init(
    Page *page,
    u8* base,
    size_t capacity,
    PageState state,
    PageAge age,
    PageSpace space) {
  page->base = base;
  page->capacity = capacity;
  page->remembered_set.slots = NULL;
  page->remembered_set.count = 0;
  page->remembered_set.capacity = 0;
  page->forwarding = NULL;
  page->forwarding_count = 0;
  page->forwarding_capacity = 0;
  page_reset(page, state, age, space);
}

/* Reinitialize a retained page for a new state, age, and space. */
void page_reset(Page *page, PageState state, PageAge age, PageSpace space) {
  page->top = page->base;
  page->limit = page->base + page->capacity;
  page->used = 0;
  page->state = state;
  page->age = age;
  page->space = space;
  livemap_reset(&page->livemap);
  page_clear_remembered_set(page);
  page_clear_forwarding(page);
}

/* Reclassify a surviving page as old while preserving large space. */
void page_promote(Page* page) {
  page->age = GC_PAGE_AGE_OLD;
  page->space = page->state == GC_PAGE_LARGE ? GC_SPACE_LARGE : GC_SPACE_OLD;
}

/* Free a page's backing memory and all auxiliary metadata. */
void page_release(Page* page) {
  page_clear_forwarding(page);
  page_release_remembered_set(page);
  free(page->base);
  page->base = NULL;
  page->top = NULL;
  page->limit = NULL;
  page->used = 0;
  page->capacity = 0;
  page->state = GC_PAGE_FREE;
  page->age = GC_PAGE_AGE_YOUNG;
  page->space = GC_SPACE_NURSERY;
  livemap_reset(&page->livemap);
}

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "livemap.h"
#include "macros.h"
#include "object_header.h"

enum {
  GC_RELOCATION_LIVE_RATIO_SHIFT = 2,
};

static void gc_assert_relocation_page(const Page* page) {
  assert(page->state == GC_PAGE_RELOCATING);
}

static void gc_assert_nonrelocating_page(const Page* page) {
  assert(page->state != GC_PAGE_RELOCATING);
}

static PageForwardingEntry* page_find_forwarding(Page* page, size_t old_offset) {
  for (size_t i = 0; i < page->forwarding_count; i++) {
    if (page->forwarding[i].old_offset == old_offset) {
      return &page->forwarding[i];
    }
  }

  return NULL;
}

static bool page_add_forwarding(Page* page, size_t old_offset, u8* new_payload) {
  PageForwardingEntry* entries;
  size_t new_capacity;

  gc_assert_relocation_page(page);

  if (page->forwarding_count == page->forwarding_capacity) {
    new_capacity = page->forwarding_capacity == 0 ? 8 : page->forwarding_capacity * 2;
    entries = (PageForwardingEntry*) realloc(page->forwarding, new_capacity * sizeof(page->forwarding[0]));
    if (entries == NULL) {
      return false;
    }

    page->forwarding = entries;
    page->forwarding_capacity = new_capacity;
  }

  page->forwarding[page->forwarding_count].old_offset = old_offset;
  page->forwarding[page->forwarding_count].new_payload = new_payload;
  page->forwarding_count++;
  return true;
}

static Page* arena_get_free_normal_page(Arena* arena, size_t min_capacity) {
  for (size_t i = 0; i < arena->page_count; i++) {
    Page* page = &arena->pages[i];
    if (page->state == GC_PAGE_FREE && page->base != NULL && page->capacity >= min_capacity) {
      gc_assert_nonrelocating_page(page);
      assert(page->forwarding_count == 0);
      page_reset(page, GC_PAGE_ACTIVE);
      return page;
    }
  }
  return NULL;
}

static bool gc_forward_live_object(
    Arena* arena,
    Page* source_page,
    size_t old_offset,
    void** new_payload_out,
    Page** destination_page_out) {
  const size_t header_size = arena_make_layout(0).header_size;
  const ObjectHeader* old_header = (const ObjectHeader*) (source_page->base + old_offset);
  const AllocLayout layout = arena_make_layout(old_header->size);
  Page* destination_page;
  ObjectHeader* new_header;
  u8* new_top;
  size_t dest_offset;

  if (layout.total_size > source_page->capacity) {
    return false;
  }

  gc_assert_relocation_page(source_page);
  destination_page = arena_get_free_normal_page(arena, layout.total_size);
  if (destination_page == NULL) {
    destination_page = arena_add_page(arena, GC_PAGE_SIZE, GC_PAGE_ACTIVE);
  }

  if (destination_page->state != GC_PAGE_ACTIVE) {
    return false;
  }
  gc_assert_nonrelocating_page(destination_page);
  assert(destination_page->forwarding_count == 0);

  new_top = destination_page->top;
  dest_offset = (size_t) (new_top - destination_page->base);

  if (!page_add_forwarding(source_page, old_offset, new_top + header_size)) {
    return false;
  }

  if (!livemap_mark(&destination_page->livemap, dest_offset, layout.total_size)) {
    source_page->forwarding_count--;
    return false;
  }

  new_header = (ObjectHeader*) new_top;
  memcpy(new_header, old_header, layout.total_size);
  destination_page->top += layout.total_size;
  destination_page->used += layout.total_size;

  *new_payload_out = (void*) (new_top + header_size);
  if (destination_page_out != NULL) {
    *destination_page_out = destination_page;
  }
  return true;
}

void* gc_forward_if_relocating(Arena* arena, void* object) {
  Page* source_page;
  const size_t header_size = arena_make_layout(0).header_size;
  const ObjectHeader* hp;
  size_t old_offset;
  PageForwardingEntry* entry;
  void* new_payload;

  if (object == NULL) {
    return NULL;
  }

  source_page = arena_find_page(arena, object);
  if (source_page == NULL) {
    return object;
  }

  hp = get_header_pointer(object, header_size);
  old_offset = (size_t) ((const u8*) hp - source_page->base);
  entry = page_find_forwarding(source_page, old_offset);
  if (entry != NULL) {
    return entry->new_payload;
  }

  if (source_page->state != GC_PAGE_RELOCATING) {
    return object;
  }

  if (!gc_forward_live_object(arena, source_page, old_offset, &new_payload, NULL)) {
    return NULL;
  }

  return new_payload;
}

static bool gc_page_is_sparse(Page* page) {
  if (page->state != GC_PAGE_ACTIVE && page->state != GC_PAGE_FULL) {
    return false;
  }

  return page->livemap.live_bytes > 0 &&
      page->livemap.live_bytes <= (page->capacity >> GC_RELOCATION_LIVE_RATIO_SHIFT);
}

static bool gc_evacuate_page(Arena* arena, Page* source_page) {
  source_page->state = GC_PAGE_RELOCATING;
  assert(source_page->forwarding_count == 0);

  for (u8* cursor = source_page->base; cursor < source_page->top; ) {
    ObjectHeader* header = (ObjectHeader*) cursor;
    size_t old_offset = (size_t) (cursor - source_page->base);
    u8* new_payload;
    Page* destination_page = NULL;

    if (livemap_is_live(&source_page->livemap, old_offset)) {
      if (!gc_forward_live_object(arena, source_page, old_offset, (void**) &new_payload, &destination_page)) {
        for (size_t i = 0; i < arena->page_count; i++) {
          Page* page = &arena->pages[i];

          if (page->state == GC_PAGE_ACTIVE &&
              page->capacity == GC_PAGE_SIZE &&
              page->used == 0) {
            gc_assert_nonrelocating_page(page);
            page_reset(page, GC_PAGE_FREE);
          }
        }

        source_page->state = GC_PAGE_FULL;
        page_clear_forwarding(source_page);
        return false;
      }
    }

    cursor += header->total_size;
  }

  return true;
}

bool gc_verify_relocation(Arena* arena) {
  const size_t header_size = arena_make_layout(0).header_size;

  for (size_t i = 0; i < arena->page_count; i++) {
    Page* source_page = &arena->pages[i];

    if (source_page->state != GC_PAGE_RELOCATING) {
      continue;
    }

    for (size_t j = 0; j < source_page->forwarding_count; j++) {
      const PageForwardingEntry* entry = &source_page->forwarding[j];
      const ObjectHeader* old_header = (const ObjectHeader*) (source_page->base + entry->old_offset);
      Page* destination_page = arena_find_page(arena, entry->new_payload);
      const ObjectHeader* new_header;
      size_t destination_offset;

      if (destination_page == NULL || destination_page == source_page) {
        return false;
      }

      new_header = get_header_pointer(entry->new_payload, header_size);
      destination_offset = (size_t) ((const u8*) new_header - destination_page->base);

      if (new_header->size != old_header->size) {
        return false;
      }

      if (!livemap_is_live(&destination_page->livemap, destination_offset)) {
        return false;
      }
    }
  }

  return true;
}

void gc_finish_relocation(Arena* arena) {
  for (size_t i = 0; i < arena->page_count; i++) {
    Page* page = &arena->pages[i];

    if (page->state == GC_PAGE_RELOCATING) {
      assert(page->forwarding_count == 0 || page->forwarding != NULL);
      page_reset(page, GC_PAGE_FREE);
    }
  }
}

bool gc_evacuate_sparse_pages(Arena* arena, const GCRootSet* roots) {
  (void) roots;

  const size_t initial_page_count = arena->page_count;

  for (size_t i = 0; i < initial_page_count; i++) {
    Page* source_page = &arena->pages[i];

    if (!gc_page_is_sparse(source_page)) {
      continue;
    }

    if (!gc_evacuate_page(arena, source_page)) {
      return false;
    }
  }

  return true;
}

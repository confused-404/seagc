#ifndef ARENA_H
#define ARENA_H

#include "config.h"
#include "types.h"
#include "page.h"
#include "object_header.h"

typedef struct RememberedSet {
  void*** slots;
  size_t count;
  size_t capacity;
} RememberedSet;

typedef struct RootRegistry {
  void*** slots;
  size_t count;
  size_t capacity;
} RootRegistry;

typedef struct Arena {
  Page pages[GC_MAX_PAGES];
  size_t page_count;
  Page* young_active_page;
  Page* old_active_page;
  RememberedSet remembered_set;
  RootRegistry roots;
} Arena;

typedef struct AllocLayout {
  size_t header_size;
  size_t total_size;
} AllocLayout;

typedef void (*ArenaObjectVisitor)(
    Page* page,
    const ObjectHeader* header,
    void* payload,
    void* user_data);

typedef bool (*ArenaObjectFieldVisitor)(
    const ObjectHeader* header,
    void* payload,
    void** field_slot,
    void* user_data);

AllocLayout arena_make_layout(size_t payload_size);
void arena_init(Arena* arena);
void arena_destroy(Arena* arena);
Page* arena_add_page(Arena* arena, size_t capacity, PageState state, PageAge age);
Page* arena_get_active_page_for_age(Arena* arena, size_t size, PageAge age);
void* arena_alloc(Arena* arena, size_t payload_size);
void* arena_alloc_traced(Arena* arena, size_t payload_size, const TraceDescriptor* trace);
bool arena_should_collect(const Arena* arena);
Page* arena_find_page(Arena* arena, const void* payload_pointer);
bool arena_mark_object(Arena* arena, const void* payload_pointer);
void arena_mark_object_fields(Arena* arena, void* payload_pointer);
bool arena_visit_object_fields(
    Arena* arena,
    void* payload_pointer,
    ArenaObjectFieldVisitor visitor,
    void* user_data);
void arena_for_each_object(Arena* arena, ArenaObjectVisitor visitor, void* user_data);

const ObjectHeader* get_header_pointer(const void* payload_pointer, size_t header_size);

#endif

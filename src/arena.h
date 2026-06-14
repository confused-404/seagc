#ifndef ARENA_H
#define ARENA_H

#include "config.h"
#include "types.h"
#include "page.h"
#include "object_header.h"

typedef struct RootRegistry {
  void*** slots;
  size_t count;
  size_t capacity;
} RootRegistry;

typedef enum GCCollectionReason {
  GC_REASON_NONE = 0,
  GC_REASON_ALLOCATION_NURSERY_PRESSURE,
  GC_REASON_ALLOCATION_FAILURE,
  GC_REASON_OLD_SPACE_PRESSURE,
  GC_REASON_EXPLICIT_YOUNG,
  GC_REASON_EXPLICIT_FULL,
} GCCollectionReason;

typedef struct ArenaGCPolicy {
  size_t nursery_page_target;
  size_t max_nursery_pages;
  u8 promotion_age;
  size_t full_page_watermark;
} ArenaGCPolicy;

typedef struct ArenaGCStats {
  size_t allocated_bytes[GC_SPACE_LARGE + 1u];
  size_t live_bytes[GC_SPACE_LARGE + 1u];
  size_t copied_bytes;
  size_t promoted_bytes;
  size_t reclaimed_bytes;
  size_t minor_collections;
  size_t full_collections;
  GCCollectionReason last_collection_reason;
} ArenaGCStats;

typedef struct Arena {
  Page pages[GC_MAX_PAGES];
  size_t page_count;
  Page* nursery_active_page;
  Page* survivor_active_page;
  Page* old_active_page;
  RootRegistry roots;
  ArenaGCPolicy policy;
  ArenaGCStats stats;
} Arena;

typedef enum ArenaCollectionTrigger {
  GC_TRIGGER_NONE = 0,
  GC_TRIGGER_YOUNG,
  GC_TRIGGER_FULL,
} ArenaCollectionTrigger;

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
Page* arena_add_page(
    Arena* arena,
    size_t capacity,
    PageState state,
    PageAge age,
    PageSpace space);
Page* arena_get_active_page_for_age(Arena* arena, size_t size, PageAge age);
Page* arena_get_active_page_for_space(Arena* arena, size_t size, PageSpace space);
void* arena_alloc(Arena* arena, size_t payload_size);
void* arena_alloc_traced(Arena* arena, size_t payload_size, const TraceDescriptor* trace);
ArenaCollectionTrigger arena_collection_trigger(const Arena* arena);
bool arena_should_collect(const Arena* arena);
const ArenaGCPolicy* gc_policy(const Arena* arena);
const ArenaGCStats* gc_stats(const Arena* arena);
void arena_stats_recompute_live(Arena* arena);
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

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

/* Compute the aligned header and total allocation sizes for a payload. */
AllocLayout arena_make_layout(size_t payload_size);
/* Initialize an empty arena with the default collection policy. */
void arena_init(Arena* arena);
/* Release every resource owned by an arena. */
void arena_destroy(Arena* arena);
/* Append a page with newly allocated backing memory. */
Page* arena_add_page(
    Arena* arena,
    size_t capacity,
    PageState state,
    PageAge age,
    PageSpace space);
/* Find or create the normal allocation page implied by an age. */
Page* arena_get_active_page_for_age(Arena* arena, size_t size, PageAge age);
/* Find or create an active normal page in a specific space. */
Page* arena_get_active_page_for_space(Arena* arena, size_t size, PageSpace space);
/* Allocate a pointer-free object directly in nursery space. */
void* arena_alloc(Arena* arena, size_t payload_size);
/* Allocate an exactly traced object directly in nursery space. */
void* arena_alloc_traced(Arena* arena, size_t payload_size, const TraceDescriptor* trace);
/* Low-level allocator for GC internals/tests that must choose a generation space. */
void* arena_alloc_traced_in_space(
    Arena* arena,
    size_t payload_size,
    const TraceDescriptor* trace,
    PageSpace space);
/* Choose the collection requested by current allocation pressure. */
ArenaCollectionTrigger arena_collection_trigger(const Arena* arena);
/* Report whether current pressure requests any collection. */
bool arena_should_collect(const Arena* arena);
/* Return the arena's current collection policy. */
const ArenaGCPolicy* gc_policy(const Arena* arena);
/* Return the arena's current collection statistics. */
const ArenaGCStats* gc_stats(const Arena* arena);
/* Recompute live-byte statistics from non-source pages. */
void arena_stats_recompute_live(Arena* arena);
/* Find the non-free page containing an object's header. */
Page* arena_find_page(Arena* arena, const void* payload_pointer);
/* Mark an object and report whether this is its first mark. */
bool arena_mark_object(Arena* arena, const void* payload_pointer);
/* Mark every object directly referenced by a traced payload. */
void arena_mark_object_fields(Arena* arena, void* payload_pointer);
/* Visit each pointer slot described by an object's trace metadata. */
bool arena_visit_object_fields(
    Arena* arena,
    void* payload_pointer,
    ArenaObjectFieldVisitor visitor,
    void* user_data);
/* Walk every allocated object in page and allocation order. */
void arena_for_each_object(Arena* arena, ArenaObjectVisitor visitor, void* user_data);

/* Recover an object's header from its payload pointer. */
const ObjectHeader* get_header_pointer(const void* payload_pointer, size_t header_size);

#endif

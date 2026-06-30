# SeaGC

SeaGC is a from-scratch garbage collector experiment in C. The long-term goal
is a concurrent, generational, ZGC-like collector. The current code is a small
stop-the-world exact generational collector built around pages.

## Current State

Implemented:

- Page-based allocation with normal and large-object pages
- Exact tracing via `TraceDescriptor`
- Explicit root sets and transitive marking
- Page-local livemaps
- Sweeping and page reuse
- Sparse-page evacuation with forwarding tables and pointer repair
- Nursery, survivor, old, and large page spaces
- Young collection with object aging and promotion
- Full collection that marks the whole heap, evacuates sparse pages, promotes
  surviving young pages, and rebuilds remembered sets
- Direct old-generation allocation through `gc_alloc_old` and
  `gc_alloc_old_traced`
- Old-to-young write barriers through `GC_STORE`, `GC_STORE_SLOT`, and
  `gc_store_pointer`
- Remembered sets for precise minor collection
- Load/remap barrier API through `GC_LOAD_SLOT` and `gc_load_pointer`
- Failure rollback for allocation, barriers, and relocation batches
- GC-aware allocation retry through `gc_alloc` and `gc_alloc_old`

Not implemented:

- Stack or conservative roots
- Partial-page reclamation without evacuation
- Concurrent marking or relocation
- Weak references or finalizers

## Generational Policy

Normal allocations enter the nursery. Large allocations enter large-object pages,
which start young for normal allocation and can be allocated old with
`gc_alloc_old`. Objects that survive minor collection are copied to survivor
pages with their header age incremented. Once an object reaches
`GC_PROMOTION_AGE`, young evacuation promotes it to old pages. Live young large
objects are promoted in place.

`arena_collection_trigger` drives automatic collection before allocation:
nursery page pressure triggers a minor collection, while low remaining page
capacity triggers a full collection. If an allocation still fails, `gc_alloc`
first retries after a minor collection and then after a full collection.
`gc_alloc_old` skips nursery-pressure minor collection and uses full collection
for old-space pressure or allocation failure.

Minor collection is precise: it clears young livemaps, marks young roots, marks
young objects reachable from remembered old slots, evacuates live young pages,
repairs roots and remembered slots, repairs fields inside relocated young
objects, finishes relocation, and prunes stale remembered-set entries. It does
not scan the whole old generation except in debug verification.

Full collection is stop-the-world and separated into mark, sweep-dead,
evacuate, verify-forwarding, repair-roots, repair-objects, finish-relocation,
final-sweep, promote, and remembered-set rebuild phases. Sparse pages use
forwarding tables as the relocation indirection; repaired roots and fields are
remapped to the forwarded payloads before source pages are released.

## Barrier Contract

Any mutator store that can write a GC pointer into a heap object must use
`GC_STORE`, `GC_STORE_SLOT`, or `gc_store_pointer`. These APIs record an
old-to-young edge before publishing the new slot value, so a remembered-set
allocation failure leaves the original slot unchanged. Direct C field writes are
not intercepted and can break minor collection; `gc_verify_remembered_set`
exists to catch this in tests/debug builds.

`GC_LOAD_SLOT` and `gc_load_pointer` provide a stop-the-world remap/load barrier
at API boundaries. They update a slot if it still points at a relocating object
with an existing or newly-created forwarding entry, and return `NULL` if
forwarding a relocating object fails. SeaGC does not currently run relocation
concurrently with mutators, so these load barriers are explicit API helpers
rather than compiler- or hardware-enforced read barriers.

## Build

```sh
make
./bin/seagc
```

The executable runs smoke tests and prints collector state.

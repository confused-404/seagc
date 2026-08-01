# SeaGC Architecture

This document describes the current SeaGC implementation. SeaGC is a
stop-the-world, exact, generational collector organized around page-local
allocation, livemaps, remembered sets, and forwarding tables.

The collector has no concurrent mutator/collector protocol yet. Collection is
entered explicitly by the allocation APIs or by `gc_collect_young` and
`gc_collect`, and the caller is responsible for using the pointer-store and
pointer-load APIs at the documented boundaries.

## Ownership model

The ownership hierarchy is:

```text
Arena
├── Page descriptors
│   ├── page memory containing ObjectHeaders and payloads
│   ├── LiveMap
│   ├── RememberedSet
│   └── relocation forwarding table
└── RootRegistry

ObjectHeader ──borrows──> TraceDescriptor
RememberedSet ──borrows addresses of──> heap pointer slots
RootRegistry ──borrows addresses of──> root slots
```

The collector owns metadata containers and page memory. It does not own the
storage for application root slots, the storage of a `TraceDescriptor`, or the
payload objects referenced by root and remembered slots.

## `ObjectHeader`

Defined in [`src/object_header.h`](src/object_header.h).

### Responsibility

An `ObjectHeader` describes the payload immediately following it in page
memory. It lets the collector walk objects, calculate their size, trace their
pointer fields, and age them through young collections.

### Fields

- `size`: unaligned application payload size.
- `total_size`: aligned size of the header plus aligned payload.
- `trace`: borrowed pointer-field metadata.
- `age`: number of successful young survivals represented by this object.

### Owner and mutators

- The owning `Page` owns the memory containing the header.
- `arena_alloc_traced_in_space` initializes the header during allocation.
- `gc_forward_live_object` copies the header during evacuation and may update
  `age` for the destination copy.
- The collector reads the header while walking, marking, tracing, sweeping, and
  repairing objects.
- Application code must treat the header as collector metadata and must not
  overwrite it through the payload interface.

### Lifetime

The header begins its lifetime when allocation commits the object to a page.
It remains valid while the object is in that page and becomes invalid when the
page is reset or released. During relocation, the source header remains
readable until relocation finishes; the destination header becomes the live
header after pointers are repaired.

### Valid states and invariants

- The header starts at an `GC_ALIGNMENT`-aligned address.
- `total_size` equals `arena_make_layout(size).total_size`.
- `total_size` is at least the aligned header size and fits wholly in the
  owning page.
- `trace` is non-NULL. Untyped objects use the internal zero-field descriptor.
- Every trace offset points to a complete `void *` within the payload.
- `age` is incremented only for objects copied during young evacuation. A copy
  promoted to old space receives the promotion age selected by the policy.
- A header is discoverable from its public payload pointer by subtracting the
  aligned header size.

## `Page`

Defined in [`src/page.h`](src/page.h), initialized and managed by
[`src/page.c`](src/page.c).

### Responsibility

A `Page` is the unit of allocation, liveness accounting, remembered-set
ownership, and relocation. Normal pages contain multiple objects; a large page
normally contains one large object.

### Owner and mutators

- The `Arena` owns the fixed array of `Page` descriptors.
- The `Page` owns its `base` allocation while `base != NULL`.
- `arena_add_page` creates page memory and initializes a descriptor.
- `arena_get_active_page_for_space` selects or reuses normal pages.
- `arena_alloc_normal` and `arena_alloc_large` advance `top` and `used`.
- `page_reset` returns a page to a reusable state and clears its livemap,
  remembered-set count, and forwarding table.
- `page_promote` changes the page age and normal/large space classification.
- `page_release` frees page memory and auxiliary arrays.
- Collection code in `gc.c` and `gc_relocate.c` changes page state, active-page
  ownership, and relocation metadata.

### Lifetime

The descriptor exists from `arena_init` until `arena_destroy`. Its backing
memory can be allocated, retained while free, reused, and finally released.
`GC_PAGE_FREE` therefore does not necessarily mean that `base` is NULL; free
normal pages are retained for reuse.

### Valid states

- `GC_PAGE_FREE`: not part of the live heap and available for reuse.
- `GC_PAGE_ACTIVE`: current bump-allocation page for nursery, survivor, or old
  normal space.
- `GC_PAGE_FULL`: normal page with no room for the requested allocation.
- `GC_PAGE_RELOCATING`: source page whose live objects are being copied and
  whose forwarding table is active.
- `GC_PAGE_LARGE`: page dedicated to a large object.

### Invariants

- `base <= top <= limit`, with `limit == base + capacity` whenever the page is
  initialized.
- `used` describes committed object bytes; for normal pages it advances with
  allocation, and for large pages it is the object allocation size.
- A normal active page has a matching active-page pointer in the `Arena` for its
  space.
- A large page is not used as a bump-allocation page.
- A free page has no live objects, no active allocation role, and empty
  remembered and forwarding counts.
- A relocating page is not an allocation destination and is not referenced by
  an active-page pointer.
- `forwarding_count == 0` implies that no forwarding entries exist; a positive
  count requires a non-NULL forwarding array.
- Only old pages in active, full, or large states may own remembered slots.

## `Arena`

Defined in [`src/arena.h`](src/arena.h) and implemented primarily by
[`src/arena.c`](src/arena.c).

### Responsibility

`Arena` is the heap and collector control object. It owns page descriptors,
active allocation cursors, persistent roots, adaptive policy, and statistics.

### Owner and mutators

- The embedding application owns the `Arena` object and controls its
  `arena_init`/`arena_destroy` lifetime.
- `arena.c` mutates pages and allocation cursors during allocation.
- `gc.c` mutates policy, statistics, root registry state, active-page pointers,
  livemaps, remembered sets, and collection state.
- `gc_relocate.c` mutates relocation destinations, forwarding tables, page
  states, active-page pointers, and relocation-related statistics.

### Lifetime

The caller initializes the `Arena` before any allocation or root registration.
All page, root, remembered-set, and forwarding metadata owned by the arena is
released by `arena_destroy`.

### Invariants

- `page_count <= GC_MAX_PAGES`.
- Every live page descriptor lies in `pages[0..page_count)`.
- Every non-NULL active-page pointer refers to a page in the arena with the
  expected active state, age, and space.
- Pages are the only owners of heap object memory.
- The root registry remains valid for the arena lifetime.
- Collection statistics and policy are updated only after successful collection
  steps; relocation rollback restores them if a relocation batch fails.

## `TraceDescriptor`

Defined in [`src/trace_descriptor.h`](src/trace_descriptor.h).

### Responsibility

A `TraceDescriptor` makes an object exact-traceable by listing the payload
offsets that contain GC pointers. SeaGC never scans arbitrary payload bytes for
possible pointers.

### Owner and mutators

- The application normally owns the descriptor, commonly as static read-only
  data next to its object type.
- `arena_alloc_traced_in_space` validates and stores the descriptor pointer in
  each matching `ObjectHeader`; it does not copy the descriptor.
- `arena_visit_object_fields`, marking, repair, remembered-set verification,
  and remembered-set rebuilding read the descriptor.
- The descriptor must not be mutated while any object refers to it.

### Lifetime

The descriptor must outlive every object header that borrows it. Static
descriptors are therefore the usual form. A descriptor for an object type can
be reclaimed only after all objects of that type have been reclaimed.

### Invariants

- A zero-pointer descriptor may have `pointer_offsets == NULL`.
- A nonzero `pointer_count` requires a non-NULL `pointer_offsets` array.
- Each offset must satisfy:

  ```text
  offset <= payload_size
  payload_size - offset >= sizeof(void *)
  ```

- Offsets are relative to the payload, not the object header.
- The descriptor must accurately describe the object layout; the collector
  cannot detect a missing or incorrectly classified pointer field by itself.

## `RootRegistry`

Defined in [`src/arena.h`](src/arena.h) and managed by root APIs in
[`src/gc.c`](src/gc.c).

### Responsibility

The root registry stores persistent addresses of root slots. These slots are
included in marking and are repaired after relocation.

The registry is separate from the per-call `GCRootSet`, which is supplied by a
caller for a particular collection or allocation retry.

### Owner and mutators

- The `Arena` owns the registry structure and its dynamically allocated
  `slots` array.
- `gc_root_register` grows and appends to the array.
- `gc_root_unregister` removes entries.
- `GCHandle` initialization and destruction use the registry indirectly by
  registering or unregistering `&handle->slot`.
- Collection reads registered roots during marking and updates the referenced
  slot values during repair; it does not replace registry entries.

### Lifetime

The registry begins empty after `arena_init` and lasts until `arena_destroy`.
Each registered slot address must remain valid until it is unregistered.

### Invariants

- `count <= capacity`.
- If `capacity > 0`, `slots` points to storage for at least `capacity` entries.
- Every entry is non-NULL.
- Entries are deduplicated.
- Entries are addresses of root slots, not object payload pointers.
- Registered slots may contain NULL or heap pointers, but their storage must be
  writable because relocation repair updates them.

## `RememberedSet`

Defined in [`src/page.h`](src/page.h) and implemented in [`src/gc.c`](src/gc.c).

### Responsibility

A remembered set records old-generation pointer slots that currently point to
young objects. It is the source of old-to-young roots during a minor
collection, allowing minor collection to avoid tracing the entire old
generation.

### Owner and mutators

- Each `Page` owns its `RememberedSet` and its dynamically allocated `slots`
  array.
- `gc_store_pointer` adds a slot before publishing an old-to-young store.
- `gc_repair_field_visitor` may add a repaired old-to-young field.
- Full collection clears and rebuilds remembered sets by scanning old objects.
- Young collection prunes entries that no longer point to young objects.
- `page_reset` clears the count while retaining capacity; `page_release` frees
  the array.

### Lifetime

The remembered set has the same descriptor lifetime as its owning page. Its
capacity may survive page reuse, but its count is reset when the page becomes
free.

### Invariants

- The set is page-local: every remembered slot address lies between the owning
  page's `base` and `top`.
- Only old pages in active, full, or large states may own entries.
- Entries are deduplicated.
- In the steady state, every non-NULL entry points to a young object.
- A remembered set owns slot addresses, not the slots themselves and not the
  objects stored in those slots.
- The write barrier must remember the slot before writing the new value. If
  growth fails, the slot must retain its previous value.
- During repair or collection, stale entries may exist temporarily; pruning or
  rebuilding restores the steady-state invariant.

## Collection phases

The full-collection phase enum is private to [`src/gc.c`](src/gc.c). It is a
local control state, not a field stored in `Arena`; only one phase is active at
a time and no mutator is expected to run between phases.

### Full collection

| Phase | Owner/mutators | Required state and result |
|---|---|---|
| `MARK` | `gc_mark`, livemaps, mark worklist | Clears marks and marks all explicit, registered, and transitively reachable objects. No page may be relocating. |
| `SWEEP_DEAD` | `gc_sweep` | Pages with zero marked objects become free. Active-page references to them are cleared. |
| `EVACUATE` | `gc_evacuate_sparse_pages`, `gc_relocate.c` | Sparse active/full pages become relocating sources; live objects are copied and forwarding entries are created. |
| `VERIFY_FORWARDING` | `gc_verify_relocation` | Every forwarding entry must point to a different live destination object with matching size. |
| `REPAIR_ROOTS` | `gc_repair_roots` | Explicit and registered root slots are changed from source payloads to forwarded payloads. |
| `REPAIR_OBJECTS` | `gc_repair_all_objects` | Fields in all non-relocating objects are repaired using existing forwarding entries; repaired old-to-young fields are remembered. |
| `FINISH_RELOCATION` | `gc_finish_relocation` | All relocating source pages are reset to free. No live pointer may still depend on a source page. |
| `FINAL_SWEEP` | `gc_sweep`, promotion, remembered-set rebuild | Remaining dead pages are freed, surviving pages are promoted, remembered sets are rebuilt and verified, and statistics are committed. |

If allocation or forwarding fails before completion, relocation snapshots restore
page metadata, forwarding tables, active-page pointers, statistics, and any
newly appended destination pages.

### Young collection

Young collection has no separate `GCCollectPhase` enum, but follows this
sequence:

1. Verify the remembered-set contract.
2. Clear livemaps only for young pages.
3. Mark young objects from explicit and registered roots.
4. Mark young objects referenced by remembered old slots.
5. Traverse only young-to-young edges.
6. Sweep dead young pages.
7. Evacuate live young normal pages; promote young large pages in place.
8. Verify forwarding entries.
9. Repair explicit and registered roots.
10. Repair fields inside relocated young objects.
11. Repair remembered slots.
12. Finish relocation, prune remembered sets, verify them, and update policy.

Young objects copied below the promotion threshold go to survivor space. Objects
that reach the threshold go to old space. The policy can lower the promotion age
when promotion pressure is observed.

## Relocation states

Relocation is represented primarily by `PageState` and by the presence of
forwarding entries. A normal page follows this lifecycle:

```text
ACTIVE or FULL
      |
      | sparse-page selection or young evacuation
      v
RELOCATING source + ACTIVE destination pages
      |
      | forwarding verification and pointer repair
      v
source page reset to FREE
```

### Candidate

A page is a full-collection candidate when it is active or full, has live
objects, and its live bytes are no more than one quarter of capacity. Young
collection evacuates live young normal pages regardless of this sparse-page
ratio.

### Relocating source

Before copying begins, the source page is set to `GC_PAGE_RELOCATING`.

Invariants while relocating:

- The source page is not an allocation destination.
- Its old object bytes remain readable until `gc_finish_relocation`.
- Its livemap identifies which source objects need copies.
- Its forwarding table contains zero or more source-offset-to-destination
  mappings.
- Active-page pointers do not point to the source page.
- A source object may not be repaired using an absent forwarding entry during
  the normal repair phases.

### Forwarded object

Each copied object has one forwarding entry containing:

- `old_offset`: the source header offset within the relocating page.
- `new_payload`: the destination object's payload pointer.

Entries are unique by `old_offset`. The destination object is marked live in its
destination page's livemap and retains the source object's trace metadata and
payload size. Young evacuation may change its age and destination space.

### Forwarding lookup modes

- `gc_forward_existing_if_relocating` only accepts an existing entry. It is used
  during root and field repair.
- `gc_forward_if_relocating` returns an existing entry or creates one on demand.
  It is used by the explicit load barrier and supports a relocating object that
  has not yet been reached through a repaired root.

If on-demand forwarding fails, the load barrier returns `NULL` and leaves the
stale slot visible so the failure is explicit.

### Finished relocation

Relocation can finish only after all relevant roots, remembered slots, and
object fields have been repaired. `gc_finish_relocation` resets each relocating
source page to `GC_PAGE_FREE`, clears its livemap and forwarding table, and
returns it to the arena's reusable page pool.

Large young pages do not enter the relocating state during young collection;
their live object is promoted in place instead.

## Pointer-store and pointer-load contracts

All mutator stores that may place a heap pointer into a heap object must use
`GC_STORE`, `GC_STORE_SLOT`, or `gc_store_pointer`. Direct field writes bypass
remembered-set maintenance.

API boundaries that may observe a slot while relocation is active should use
`GC_LOAD_SLOT` or `gc_load_pointer`. These functions repair stale relocating
payloads when forwarding is available or can be created.

These contracts are explicit because SeaGC currently has no concurrent or
compiler-enforced barrier implementation.

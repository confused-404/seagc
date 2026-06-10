# SeaGC

SeaGC is a from-scratch garbage collector experiment in C. The long-term goal
is a concurrent, generational, ZGC-like collector. The current code is a small
stop-the-world exact collector built around pages.

## Current State

Implemented:

- Page-based allocation with normal and large-object pages
- Exact tracing via `TraceDescriptor`
- Explicit root sets and transitive marking
- Page-local livemaps
- Sweeping and page reuse
- Sparse-page evacuation with forwarding and pointer repair
- Young collection with object aging and promotion
- Old-to-young write barriers through `GC_STORE`
- Remembered sets for minor collection
- Failure rollback for allocation, barriers, and relocation
- GC-aware allocation retry through `gc_alloc`

Not implemented:

- Stack or conservative roots
- Partial-page reclamation without evacuation
- Full generational policy beyond the current young/old page split
- Concurrent marking or relocation
- Weak references or finalizers

## Build

```sh
make
./bin/seagc
```

The executable runs smoke tests and prints collector state.

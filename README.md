# SeaGC

SeaGC is a from-scratch garbage collector experiment written in C.

My motivation for this project comes from writing my International
Baccalaureate extended essay about G1 and ZGC. SeaGC is a way to turn that
research into a working collector and learn the engineering tradeoffs directly.

The long-term goal is to build toward an optimized, concurrent, generational,
ZGC-like collector. The current version is intentionally much simpler: it is a
small stop-the-world mark-sweep collector used to learn and validate the basic
heap, page, marking, and sweeping machinery.

## Current Status

Implemented:

- Page-based heap allocation
- Bump allocation inside normal pages
- Dedicated pages for large objects
- Object headers with size and trace-layout metadata
- Exact tracing through explicit `TraceDescriptor` pointer offsets
- Explicit root sets
- Transitive marking with an explicit mark worklist
- Page-local livemaps with live object and byte counts
- Sweeping of fully dead normal pages
- Sweeping and reuse of dead large-object pages
- GC-aware allocation retry through `gc_alloc`

Not implemented yet:

- Stack scanning
- Conservative roots
- Partial-page object reclamation
- Compaction / evacuation
- Forwarding pointers
- Generational collection
- Concurrent marking or relocation
- Write barriers / remembered sets
- Weak references or finalizers

## Design Direction

The collector is organized around fixed-size pages. Normal objects are packed
into shared pages, while large objects receive dedicated pages. Liveness is
tracked per page using a livemap. The current collector only reclaims whole dead
pages; partially live pages are left in place.

The intended future direction is:

1. Build a correct primitive mark-sweep collector.
2. Add relocation and forwarding metadata.
3. Add generations and remembered sets.
4. Add concurrent marking/relocation ideas inspired by ZGC.

## Build

```sh
make
./bin/seagc
```

The executable currently runs smoke tests and prints allocator/collector state.

## Project State

This is a learning project and is not production-ready. Development is paused
temporarily while I finish exams; the next major work will focus on making the
collector more concurrent and eventually generational.

## References

- <https://github.com/openjdk/zgc>
- <https://www.microsoft.com/en-us/research/wp-content/uploads/2019/06/mimalloc-tr-v1.pdf>
- <https://man7.org/linux/man-pages/man2/>
- <https://memory-pool-system.readthedocs.io/en/latest/guide/overview.html>

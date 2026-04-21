# SeaGC

(name is just "C" GC)

## Initial Design Plan

### Starting Declarations

- Heap is divided into fixed-size pages
- Every object belongs to exactly one page
- Liveness is tracked per page
- Relocation happens at page level
- Moved objects are found through forwarding, not global pointer patching

### Heap Address Model

- Fixed heap reservation
- Fixed page size
- Address-to-page lookup: page table
- Normal objects are packed into shared pages
- Large objects get dedicated pages

### Page Abstraction

- Memory span
- Allocation top
- Allocation limit
- Liveness metadata
- State flags (free/active/relocating)
- Size class

Behavior:

- Small and medium objects are allocated with a bump pointer inside an active page
- When a page fills, allocation moves to another page of the same size class
- Pages transition through free, active, full, relocating, and reusable states
- Fully dead pages are reclaimed directly
- Partially live pages may be selected for evacuation

### Allocation Model

- SeaGC v1 uses bump allocation inside a page
- This is a collector-oriented choice, not a general-purpose malloc/free choice
- The fast path should only decide object size class, find an active page, and bump `top`
- Page selection and OS interaction belong to the slower page allocator path
- Allocation is page-local, not one global bump pointer for the entire heap

### Large Object Policy

- Objects above a threshold get dedicated pages
- The threshold should be defined relative to page size
- Large objects are still tracked through the same page table
- Large-object pages can be reclaimed directly when dead
- Large-object compaction can be delayed until the normal relocation path works

### Object Metadata Placement

- Initial direction: hybrid
- Each object has a small header with enough data to find its size and scan layout
- Each page owns side metadata for liveness and relocation support
- Pointer-coloring is not a v1 goal
- Any ZGC-like state needed early should live in side metadata, not tagged pointers

### Liveness

- Page-local
- Represent liveness with page-local bitmaps or slot maps
- Mark object starts, not arbitrary interior addresses
- Track live bytes per page
- Reclaim dead pages cheaply
- Choose relocation candidates by live ratio
- Think about fragmentation locally

### Roots

- v1 root handling is stop-the-world
- Roots include explicit runtime roots, stack roots, and global roots owned by the runtime
- Root discovery must be defined before implementing marking
- Root updates happen directly during stop-the-world phases
- Self-healing of roots can wait until after heap-slot healing works

### Marking

- v1 marking is simpler than ZGC: stop-the-world or mostly stop-the-world first
- Start with exact object graph traversal over known object layouts
- Marking work should update per-page liveness and live-byte counters
- Concurrent marking is a later phase, after the page model and relocation model work
- Weak references and finalization are out of scope for v1

### Forwarding

- Forwarding is page-scoped, not one global forwarding table
- A relocating page gets forwarding metadata for objects moved out of it
- Forwarding lookup maps old object location to new object location
- A forwarding entry is installed only after the copied object is fully initialized
- If multiple threads try to move the same object later, only one forwarding target wins
- Stale references are repaired through forwarding lookup, not eager whole-heap patching

### Relocation

- Relocation operates on selected source pages
- Fully dead pages are reclaimed without evacuation
- Partially live pages below a live-ratio threshold are candidates for evacuation
- Live objects are copied into destination pages of the correct size class
- Once all needed references are repaired, old source pages become reclaimable

### GC Cycle

- Allocate into active pages
- Trigger GC when free space or allocation pressure crosses a threshold
- Stop the world and capture roots
- Mark reachable objects and update per-page liveness
- Reclaim fully dead pages
- Select sparse live pages as a relocation set
- Evacuate live objects and install forwarding entries
- Reclaim evacuated source pages

### v1 Non-Goals

- No generations yet
- No old-to-young remembered sets
- No weak references or finalizers
- No tagged pointers
- No concurrent relocation until the basic relocation path works
- No sophisticated OS commit/uncommit heuristics yet

## Where I got my info from

<https://github.com/openjdk/zgc>

<https://www.microsoft.com/en-us/research/wp-content/uploads/2019/06/mimalloc-tr-v1.pdf>

<https://man7.org/linux/man-pages/man2/>

<https://stackoverflow.com/questions/52196678/what-are-atomic-operations-for-newbies>

<https://medium.com/inspiredbrilliance/atomic-operations-explained-from-hardware-to-high-level-code-using-go-742347d094f6>

<https://memory-pool-system.readthedocs.io/en/latest/guide/overview.html>

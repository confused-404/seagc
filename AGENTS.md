# Project agent memory

This file is the project's committed home for project-intrinsic agent knowledge: build, test, release, architecture, and sharp-edge notes that should travel with the code.

- Add durable project-specific notes here as they are discovered through real work.
- Build and smoke-test SeaGC with `make clean && make && ./bin/seagc`; the
  Makefile uses strict `-Werror` C99 flags.
- Build benchmarks with `make bench`; `./bin/seagc_bench --quick --csv` is the
  fast benchmark smoke path.
- Heap object pointer stores must use `GC_STORE`, `GC_STORE_SLOT`, or
  `gc_store_pointer` so old-to-young edges are recorded for minor collection.
- Use `GC_LOAD_SLOT` or `gc_load_pointer` at API boundaries that may observe a
  slot while relocation is in progress; they repair stale relocating pointers.

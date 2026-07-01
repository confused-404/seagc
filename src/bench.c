#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arena.h"
#include "config.h"
#include "gc.h"
#include "macros.h"

typedef struct BenchNode {
  GCPtr left;
  GCPtr right;
  GCPtr next;
  GCPtr extra;
  size_t value;
} BenchNode;

typedef struct BenchWide {
  GCPtr slots[16];
  size_t value;
} BenchWide;

typedef struct PageCounts {
  size_t free_pages;
  size_t nursery_pages;
  size_t survivor_pages;
  size_t old_pages;
  size_t large_pages;
} PageCounts;

typedef struct BenchOptions {
  size_t iterations;
  size_t repeat;
  size_t scale;
  size_t live_set;
  size_t churn;
  unsigned int seed;
  const char* workload;
  bool csv;
  bool quick;
} BenchOptions;

typedef struct BenchReport {
  const char* workload;
  size_t repeat_index;
  size_t units;
  size_t alloc_count;
  size_t approx_bytes;
  size_t minor_collections;
  size_t full_collections;
  size_t page_count;
  size_t remembered_slots;
  size_t live_bytes;
  PageCounts pages;
  double elapsed_seconds;
  size_t checksum;
} BenchReport;

typedef bool (*BenchWorkloadFn)(const BenchOptions* options, size_t repeat_index, BenchReport* report);

typedef struct BenchWorkload {
  const char* name;
  const char* description;
  BenchWorkloadFn run;
} BenchWorkload;

static const size_t bench_node_offsets[] = {
  offsetof(BenchNode, left),
  offsetof(BenchNode, right),
  offsetof(BenchNode, next),
  offsetof(BenchNode, extra),
};

static const TraceDescriptor bench_node_trace = {
  .pointer_count = ARRAY_LEN(bench_node_offsets),
  .pointer_offsets = bench_node_offsets,
};

static const size_t bench_wide_offsets[] = {
  offsetof(BenchWide, slots[0]),
  offsetof(BenchWide, slots[1]),
  offsetof(BenchWide, slots[2]),
  offsetof(BenchWide, slots[3]),
  offsetof(BenchWide, slots[4]),
  offsetof(BenchWide, slots[5]),
  offsetof(BenchWide, slots[6]),
  offsetof(BenchWide, slots[7]),
  offsetof(BenchWide, slots[8]),
  offsetof(BenchWide, slots[9]),
  offsetof(BenchWide, slots[10]),
  offsetof(BenchWide, slots[11]),
  offsetof(BenchWide, slots[12]),
  offsetof(BenchWide, slots[13]),
  offsetof(BenchWide, slots[14]),
  offsetof(BenchWide, slots[15]),
};

static const TraceDescriptor bench_wide_trace = {
  .pointer_count = ARRAY_LEN(bench_wide_offsets),
  .pointer_offsets = bench_wide_offsets,
};

static bool run_nursery(const BenchOptions* options, size_t repeat_index, BenchReport* report);
static bool run_graph(const BenchOptions* options, size_t repeat_index, BenchReport* report);
static bool run_remembered(const BenchOptions* options, size_t repeat_index, BenchReport* report);
static bool run_promotion(const BenchOptions* options, size_t repeat_index, BenchReport* report);
static bool run_large(const BenchOptions* options, size_t repeat_index, BenchReport* report);
static bool run_mixed(const BenchOptions* options, size_t repeat_index, BenchReport* report);

static const BenchWorkload workloads[] = {
  {
    .name = "nursery",
    .description = "short-lived nursery allocation throughput",
    .run = run_nursery,
  },
  {
    .name = "graph",
    .description = "pointer-rich graph allocation and traversal",
    .run = run_graph,
  },
  {
    .name = "remembered",
    .description = "old-to-young stores and remembered-set pressure",
    .run = run_remembered,
  },
  {
    .name = "promotion",
    .description = "survivor pressure and promotion over minor collections",
    .run = run_promotion,
  },
  {
    .name = "large",
    .description = "large-object allocation and full collection behavior",
    .run = run_large,
  },
  {
    .name = "mixed",
    .description = "configurable live set with allocation churn",
    .run = run_mixed,
  },
};

static double bench_now_seconds(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    return (double) ts.tv_sec + ((double) ts.tv_nsec / 1000000000.0);
  }

  return (double) clock() / (double) CLOCKS_PER_SEC;
}

static size_t checked_mul_or_max(size_t left, size_t right) {
  if (right != 0u && left > SIZE_MAX / right) {
    return SIZE_MAX;
  }

  return left * right;
}

static size_t effective_iterations(const BenchOptions* options) {
  return checked_mul_or_max(options->iterations, options->scale);
}

static size_t scaled_count(size_t base, const BenchOptions* options) {
  return checked_mul_or_max(base, options->scale);
}

static size_t max_size(size_t left, size_t right) {
  return left > right ? left : right;
}

static void bench_record_alloc(BenchReport* report, size_t payload_size) {
  report->alloc_count++;
  report->approx_bytes += arena_make_layout(payload_size).total_size;
}

static bool bench_handle_array_init(Arena* arena, GCHandle* handles, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (!gc_handle_init(arena, &handles[i], NULL)) {
      for (size_t j = 0; j < i; j++) {
        (void) gc_handle_destroy(&handles[j]);
      }
      return false;
    }
  }

  return true;
}

static void bench_handle_array_destroy(GCHandle* handles, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (handles[i].active) {
      (void) gc_handle_destroy(&handles[i]);
    }
  }
}

static void bench_count_pages(const Arena* arena, PageCounts* counts) {
  memset(counts, 0, sizeof(*counts));

  for (size_t i = 0; i < arena->page_count; i++) {
    const Page* page = &arena->pages[i];

    if (page->state == GC_PAGE_FREE) {
      counts->free_pages++;
      continue;
    }

    switch (page->space) {
      case GC_SPACE_NURSERY:
        counts->nursery_pages++;
        break;
      case GC_SPACE_SURVIVOR:
        counts->survivor_pages++;
        break;
      case GC_SPACE_OLD:
        counts->old_pages++;
        break;
      case GC_SPACE_LARGE:
        counts->large_pages++;
        break;
      default:
        break;
    }
  }
}

static size_t bench_live_bytes(const Arena* arena) {
  size_t bytes = 0;
  const ArenaGCStats* stats = gc_stats(arena);

  for (size_t i = 0; i <= (size_t) GC_SPACE_LARGE; i++) {
    bytes += stats->live_bytes[i];
  }

  return bytes;
}

static void bench_finish_report(
    Arena* arena,
    BenchReport* report,
    const ArenaGCStats* before,
    double start_seconds) {
  const ArenaGCStats* after;

  arena_stats_recompute_live(arena);
  after = gc_stats(arena);

  report->elapsed_seconds = bench_now_seconds() - start_seconds;
  report->minor_collections = after->minor_collections - before->minor_collections;
  report->full_collections = after->full_collections - before->full_collections;
  report->page_count = arena->page_count;
  report->remembered_slots = gc_remembered_set_count(arena);
  report->live_bytes = bench_live_bytes(arena);
  bench_count_pages(arena, &report->pages);
}

static bool bench_alloc_node(Arena* arena, BenchReport* report, BenchNode** node) {
  BenchNode* allocated = (BenchNode*) gc_alloc_traced(arena, sizeof(*allocated), &bench_node_trace, NULL);

  if (allocated == NULL) {
    return false;
  }

  if (!GC_STORE(arena, allocated, left, NULL) ||
      !GC_STORE(arena, allocated, right, NULL) ||
      !GC_STORE(arena, allocated, next, NULL) ||
      !GC_STORE(arena, allocated, extra, NULL)) {
    return false;
  }

  allocated->value = report->alloc_count;
  bench_record_alloc(report, sizeof(*allocated));
  *node = allocated;
  return true;
}

static unsigned int bench_lcg_next(unsigned int* state) {
  *state = (*state * 1664525u) + 1013904223u;
  return *state;
}

static bool run_nursery(const BenchOptions* options, size_t repeat_index, BenchReport* report) {
  Arena arena;
  ArenaGCStats before;
  double start;
  const size_t units = effective_iterations(options);
  const size_t payload_size = 32;

  arena_init(&arena);
  before = *gc_stats(&arena);
  memset(report, 0, sizeof(*report));
  report->workload = "nursery";
  report->repeat_index = repeat_index;
  report->units = units;

  start = bench_now_seconds();
  for (size_t i = 0; i < units; i++) {
    void* payload = gc_alloc(&arena, payload_size, NULL);

    if (payload == NULL) {
      arena_destroy(&arena);
      return false;
    }

    bench_record_alloc(report, payload_size);
    if ((i & 1023u) == 1023u && !gc_collect_young(&arena, NULL)) {
      arena_destroy(&arena);
      return false;
    }
  }

  if (!gc_collect_young(&arena, NULL)) {
    arena_destroy(&arena);
    return false;
  }

  bench_finish_report(&arena, report, &before, start);
  arena_destroy(&arena);
  return true;
}

static bool run_graph(const BenchOptions* options, size_t repeat_index, BenchReport* report) {
  Arena arena;
  ArenaGCStats before;
  GCHandle root_handle;
  GCHandle tail_handle;
  BenchNode* node;
  BenchNode* tail;
  double start;
  const size_t units = max_size(1u, effective_iterations(options) / 2u);

  arena_init(&arena);
  if (!gc_handle_init(&arena, &root_handle, NULL)) {
    arena_destroy(&arena);
    return false;
  }
  if (!gc_handle_init(&arena, &tail_handle, NULL)) {
    (void) gc_handle_destroy(&root_handle);
    arena_destroy(&arena);
    return false;
  }

  before = *gc_stats(&arena);
  memset(report, 0, sizeof(*report));
  report->workload = "graph";
  report->repeat_index = repeat_index;
  report->units = units;

  start = bench_now_seconds();
  for (size_t i = 0; i < units; i++) {
    if (!bench_alloc_node(&arena, report, &node)) {
      bench_handle_array_destroy(&root_handle, 1u);
      bench_handle_array_destroy(&tail_handle, 1u);
      arena_destroy(&arena);
      return false;
    }

    node->value = i;
    tail = (BenchNode*) gc_handle_get(&tail_handle);
    if (tail == NULL) {
      if (!gc_handle_set(&root_handle, node)) {
        bench_handle_array_destroy(&root_handle, 1u);
        bench_handle_array_destroy(&tail_handle, 1u);
        arena_destroy(&arena);
        return false;
      }
    } else if (!GC_STORE(&arena, tail, next, node)) {
      bench_handle_array_destroy(&root_handle, 1u);
      bench_handle_array_destroy(&tail_handle, 1u);
      arena_destroy(&arena);
      return false;
    }

    if (!gc_handle_set(&tail_handle, node)) {
      bench_handle_array_destroy(&root_handle, 1u);
      bench_handle_array_destroy(&tail_handle, 1u);
      arena_destroy(&arena);
      return false;
    }

    if ((i & 3u) == 3u) {
      BenchNode* root = (BenchNode*) gc_handle_get(&root_handle);

      if (root != NULL && !GC_STORE(&arena, node, left, root)) {
        bench_handle_array_destroy(&root_handle, 1u);
        bench_handle_array_destroy(&tail_handle, 1u);
        arena_destroy(&arena);
        return false;
      }
    }
  }

  node = (BenchNode*) gc_handle_get(&root_handle);
  while (node != NULL) {
    report->checksum ^= node->value + report->checksum;
    node = (BenchNode*) GC_LOAD_SLOT(&arena, &node->next);
  }

  if (!gc_collect(&arena, NULL)) {
    bench_handle_array_destroy(&root_handle, 1u);
    bench_handle_array_destroy(&tail_handle, 1u);
    arena_destroy(&arena);
    return false;
  }

  bench_finish_report(&arena, report, &before, start);
  bench_handle_array_destroy(&root_handle, 1u);
  bench_handle_array_destroy(&tail_handle, 1u);
  arena_destroy(&arena);
  return true;
}

static bool run_remembered(const BenchOptions* options, size_t repeat_index, BenchReport* report) {
  Arena arena;
  ArenaGCStats before;
  GCHandle* old_handles;
  double start;
  const size_t units = effective_iterations(options);
  const size_t old_count = max_size(8u, scaled_count(64u, options));
  const size_t young_payload_size = 48;

  old_handles = (GCHandle*) calloc(old_count, sizeof(old_handles[0]));
  if (old_handles == NULL) {
    return false;
  }

  arena_init(&arena);
  if (!bench_handle_array_init(&arena, old_handles, old_count)) {
    free(old_handles);
    arena_destroy(&arena);
    return false;
  }

  before = *gc_stats(&arena);
  memset(report, 0, sizeof(*report));
  report->workload = "remembered";
  report->repeat_index = repeat_index;
  report->units = units;

  start = bench_now_seconds();
  for (size_t i = 0; i < old_count; i++) {
    BenchWide* old_object = (BenchWide*) gc_alloc_old_traced(
        &arena,
        sizeof(*old_object),
        &bench_wide_trace,
        NULL);

    if (old_object == NULL) {
      bench_handle_array_destroy(old_handles, old_count);
      free(old_handles);
      arena_destroy(&arena);
      return false;
    }

    for (size_t j = 0; j < ARRAY_LEN(old_object->slots); j++) {
      if (!GC_STORE_SLOT(&arena, old_object, &old_object->slots[j], NULL)) {
        bench_handle_array_destroy(old_handles, old_count);
        free(old_handles);
        arena_destroy(&arena);
        return false;
      }
    }
    old_object->value = i;
    bench_record_alloc(report, sizeof(*old_object));
    if (!gc_handle_set(&old_handles[i], old_object)) {
      bench_handle_array_destroy(old_handles, old_count);
      free(old_handles);
      arena_destroy(&arena);
      return false;
    }
  }

  for (size_t i = 0; i < units; i++) {
    BenchWide* old_object;
    const size_t owner_index = i % old_count;
    const size_t slot_index = (i / old_count) % ARRAY_LEN(((BenchWide*) NULL)->slots);
    void* young = gc_alloc(&arena, young_payload_size, NULL);

    if (young == NULL) {
      bench_handle_array_destroy(old_handles, old_count);
      free(old_handles);
      arena_destroy(&arena);
      return false;
    }
    bench_record_alloc(report, young_payload_size);

    old_object = (BenchWide*) gc_handle_get(&old_handles[owner_index]);
    if (old_object == NULL ||
        !GC_STORE_SLOT(&arena, old_object, &old_object->slots[slot_index], young)) {
      bench_handle_array_destroy(old_handles, old_count);
      free(old_handles);
      arena_destroy(&arena);
      return false;
    }

    report->checksum += owner_index ^ slot_index;
    if ((i & 511u) == 511u && !gc_collect_young(&arena, NULL)) {
      bench_handle_array_destroy(old_handles, old_count);
      free(old_handles);
      arena_destroy(&arena);
      return false;
    }
  }

  bench_finish_report(&arena, report, &before, start);
  bench_handle_array_destroy(old_handles, old_count);
  free(old_handles);
  arena_destroy(&arena);
  return true;
}

static bool run_promotion(const BenchOptions* options, size_t repeat_index, BenchReport* report) {
  Arena arena;
  ArenaGCStats before;
  GCHandle* handles;
  double start;
  const size_t units = effective_iterations(options);
  const size_t live_count = max_size(16u, checked_mul_or_max(options->live_set, options->scale));
  const size_t payload_size = 256;

  handles = (GCHandle*) calloc(live_count, sizeof(handles[0]));
  if (handles == NULL) {
    return false;
  }

  arena_init(&arena);
  if (!bench_handle_array_init(&arena, handles, live_count)) {
    free(handles);
    arena_destroy(&arena);
    return false;
  }

  before = *gc_stats(&arena);
  memset(report, 0, sizeof(*report));
  report->workload = "promotion";
  report->repeat_index = repeat_index;
  report->units = units;

  start = bench_now_seconds();
  for (size_t i = 0; i < units; i++) {
    void* object = gc_alloc(&arena, payload_size, NULL);

    if (object == NULL || !gc_handle_set(&handles[i % live_count], object)) {
      bench_handle_array_destroy(handles, live_count);
      free(handles);
      arena_destroy(&arena);
      return false;
    }

    bench_record_alloc(report, payload_size);
    if ((i % max_size(16u, live_count / 4u)) == 0u && !gc_collect_young(&arena, NULL)) {
      bench_handle_array_destroy(handles, live_count);
      free(handles);
      arena_destroy(&arena);
      return false;
    }
  }

  if (!gc_collect_young(&arena, NULL) || !gc_collect_young(&arena, NULL)) {
    bench_handle_array_destroy(handles, live_count);
    free(handles);
    arena_destroy(&arena);
    return false;
  }

  bench_finish_report(&arena, report, &before, start);
  bench_handle_array_destroy(handles, live_count);
  free(handles);
  arena_destroy(&arena);
  return true;
}

static bool run_large(const BenchOptions* options, size_t repeat_index, BenchReport* report) {
  Arena arena;
  ArenaGCStats before;
  GCHandle handles[8];
  double start;
  const size_t units = max_size(1u, checked_mul_or_max(max_size(1u, options->iterations / 200u), options->scale));
  const size_t payload_size = GC_LARGE_OBJECT_SIZE + KB(4);

  arena_init(&arena);
  if (!bench_handle_array_init(&arena, handles, ARRAY_LEN(handles))) {
    arena_destroy(&arena);
    return false;
  }

  before = *gc_stats(&arena);
  memset(report, 0, sizeof(*report));
  report->workload = "large";
  report->repeat_index = repeat_index;
  report->units = units;

  start = bench_now_seconds();
  for (size_t i = 0; i < units; i++) {
    void* object = gc_alloc(&arena, payload_size, NULL);

    if (object == NULL || !gc_handle_set(&handles[i % ARRAY_LEN(handles)], object)) {
      bench_handle_array_destroy(handles, ARRAY_LEN(handles));
      arena_destroy(&arena);
      return false;
    }

    bench_record_alloc(report, payload_size);
    if ((i & 7u) == 7u && !gc_collect(&arena, NULL)) {
      bench_handle_array_destroy(handles, ARRAY_LEN(handles));
      arena_destroy(&arena);
      return false;
    }
  }

  if (!gc_collect(&arena, NULL)) {
    bench_handle_array_destroy(handles, ARRAY_LEN(handles));
    arena_destroy(&arena);
    return false;
  }

  bench_finish_report(&arena, report, &before, start);
  bench_handle_array_destroy(handles, ARRAY_LEN(handles));
  arena_destroy(&arena);
  return true;
}

static bool run_mixed(const BenchOptions* options, size_t repeat_index, BenchReport* report) {
  Arena arena;
  ArenaGCStats before;
  GCHandle* node_handles;
  GCHandle large_handles[4];
  unsigned int rng = options->seed ^ (unsigned int) repeat_index;
  double start;
  const size_t units = options->churn == 0u ? effective_iterations(options) : checked_mul_or_max(options->churn, options->scale);
  const size_t live_count = max_size(8u, checked_mul_or_max(options->live_set, options->scale));
  const size_t large_payload_size = GC_LARGE_OBJECT_SIZE + KB(2);

  node_handles = (GCHandle*) calloc(live_count, sizeof(node_handles[0]));
  if (node_handles == NULL) {
    return false;
  }

  arena_init(&arena);
  if (!bench_handle_array_init(&arena, node_handles, live_count)) {
    free(node_handles);
    arena_destroy(&arena);
    return false;
  }
  if (!bench_handle_array_init(&arena, large_handles, ARRAY_LEN(large_handles))) {
    bench_handle_array_destroy(node_handles, live_count);
    free(node_handles);
    arena_destroy(&arena);
    return false;
  }

  before = *gc_stats(&arena);
  memset(report, 0, sizeof(*report));
  report->workload = "mixed";
  report->repeat_index = repeat_index;
  report->units = units;

  start = bench_now_seconds();
  for (size_t i = 0; i < units; i++) {
    const unsigned int sample = bench_lcg_next(&rng);

    if ((sample & 63u) == 0u) {
      void* large = gc_alloc(&arena, large_payload_size, NULL);

      if (large == NULL || !gc_handle_set(&large_handles[(sample >> 8) % ARRAY_LEN(large_handles)], large)) {
        bench_handle_array_destroy(large_handles, ARRAY_LEN(large_handles));
        bench_handle_array_destroy(node_handles, live_count);
        free(node_handles);
        arena_destroy(&arena);
        return false;
      }
      bench_record_alloc(report, large_payload_size);
    } else if ((sample & 3u) == 0u) {
      const size_t payload_size = 24u + (size_t) (sample & 127u);
      void* object = gc_alloc(&arena, payload_size, NULL);

      if (object == NULL) {
        bench_handle_array_destroy(large_handles, ARRAY_LEN(large_handles));
        bench_handle_array_destroy(node_handles, live_count);
        free(node_handles);
        arena_destroy(&arena);
        return false;
      }
      bench_record_alloc(report, payload_size);
    } else {
      BenchNode* node;
      BenchNode* owner;
      const size_t node_index = (size_t) (sample >> 8) % live_count;
      const size_t owner_index = (size_t) (sample >> 17) % live_count;

      if (!bench_alloc_node(&arena, report, &node)) {
        bench_handle_array_destroy(large_handles, ARRAY_LEN(large_handles));
        bench_handle_array_destroy(node_handles, live_count);
        free(node_handles);
        arena_destroy(&arena);
        return false;
      }

      node->value = sample;
      owner = (BenchNode*) gc_handle_get(&node_handles[owner_index]);
      if (owner != NULL && !GC_STORE(&arena, owner, extra, node)) {
        bench_handle_array_destroy(large_handles, ARRAY_LEN(large_handles));
        bench_handle_array_destroy(node_handles, live_count);
        free(node_handles);
        arena_destroy(&arena);
        return false;
      }
      if (!gc_handle_set(&node_handles[node_index], node)) {
        bench_handle_array_destroy(large_handles, ARRAY_LEN(large_handles));
        bench_handle_array_destroy(node_handles, live_count);
        free(node_handles);
        arena_destroy(&arena);
        return false;
      }
      report->checksum ^= node->value;
    }

    if ((i & 1023u) == 1023u && !gc_collect_young(&arena, NULL)) {
      bench_handle_array_destroy(large_handles, ARRAY_LEN(large_handles));
      bench_handle_array_destroy(node_handles, live_count);
      free(node_handles);
      arena_destroy(&arena);
      return false;
    }
    if ((i & 8191u) == 8191u && !gc_collect(&arena, NULL)) {
      bench_handle_array_destroy(large_handles, ARRAY_LEN(large_handles));
      bench_handle_array_destroy(node_handles, live_count);
      free(node_handles);
      arena_destroy(&arena);
      return false;
    }
  }

  if (!gc_collect(&arena, NULL)) {
    bench_handle_array_destroy(large_handles, ARRAY_LEN(large_handles));
    bench_handle_array_destroy(node_handles, live_count);
    free(node_handles);
    arena_destroy(&arena);
    return false;
  }

  bench_finish_report(&arena, report, &before, start);
  bench_handle_array_destroy(large_handles, ARRAY_LEN(large_handles));
  bench_handle_array_destroy(node_handles, live_count);
  free(node_handles);
  arena_destroy(&arena);
  return true;
}

static bool parse_size_arg(const char* text, size_t* value) {
  unsigned long parsed;
  char* end = NULL;

  if (text[0] == '-') {
    return false;
  }

  errno = 0;
  parsed = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || parsed > (unsigned long) SIZE_MAX) {
    return false;
  }

  *value = (size_t) parsed;
  return true;
}

static bool parse_uint_arg(const char* text, unsigned int* value) {
  unsigned long parsed;
  char* end = NULL;

  if (text[0] == '-') {
    return false;
  }

  errno = 0;
  parsed = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || parsed > (unsigned long) UINT_MAX) {
    return false;
  }

  *value = (unsigned int) parsed;
  return true;
}

static void print_usage(FILE* stream, const char* argv0) {
  fprintf(stream, "Usage: %s [options]\n", argv0);
  fprintf(stream, "Options:\n");
  fprintf(stream, "  --workload NAME   workload to run, or all (default: all)\n");
  fprintf(stream, "  --iterations N    base iteration count per workload (default: 20000)\n");
  fprintf(stream, "  --repeat N        repeat each selected workload (default: 1)\n");
  fprintf(stream, "  --scale N         multiply workload sizes (default: 1)\n");
  fprintf(stream, "  --live-set N      base live handles for promotion/mixed (default: 256)\n");
  fprintf(stream, "  --churn N         base mixed-workload churn, 0 uses --iterations (default: 0)\n");
  fprintf(stream, "  --seed N          deterministic mixed-workload seed (default: 1)\n");
  fprintf(stream, "  --csv             print machine-readable CSV\n");
  fprintf(stream, "  --quick           short smoke-sized run\n");
  fprintf(stream, "  --list            list workloads\n");
  fprintf(stream, "  --help            show this help\n");
}

static void list_workloads(void) {
  for (size_t i = 0; i < ARRAY_LEN(workloads); i++) {
    printf("%s\t%s\n", workloads[i].name, workloads[i].description);
  }
}

static bool parse_options(int argc, char** argv, BenchOptions* options) {
  options->iterations = 20000;
  options->repeat = 1;
  options->scale = 1;
  options->live_set = 256;
  options->churn = 0;
  options->seed = 1;
  options->workload = "all";
  options->csv = false;
  options->quick = false;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--csv") == 0) {
      options->csv = true;
    } else if (strcmp(argv[i], "--quick") == 0) {
      options->quick = true;
      options->iterations = 2000;
      options->live_set = 64;
      options->churn = 2000;
    } else if (strcmp(argv[i], "--list") == 0) {
      list_workloads();
      exit(0);
    } else if (strcmp(argv[i], "--help") == 0) {
      print_usage(stdout, argv[0]);
      exit(0);
    } else if (strcmp(argv[i], "--workload") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "--workload requires a value\n");
        return false;
      }
      options->workload = argv[i];
    } else if (strcmp(argv[i], "--iterations") == 0) {
      if (++i >= argc || !parse_size_arg(argv[i], &options->iterations)) {
        fprintf(stderr, "--iterations requires a non-negative integer\n");
        return false;
      }
    } else if (strcmp(argv[i], "--repeat") == 0) {
      if (++i >= argc || !parse_size_arg(argv[i], &options->repeat) || options->repeat == 0u) {
        fprintf(stderr, "--repeat requires a positive integer\n");
        return false;
      }
    } else if (strcmp(argv[i], "--scale") == 0) {
      if (++i >= argc || !parse_size_arg(argv[i], &options->scale) || options->scale == 0u) {
        fprintf(stderr, "--scale requires a positive integer\n");
        return false;
      }
    } else if (strcmp(argv[i], "--live-set") == 0) {
      if (++i >= argc || !parse_size_arg(argv[i], &options->live_set) || options->live_set == 0u) {
        fprintf(stderr, "--live-set requires a positive integer\n");
        return false;
      }
    } else if (strcmp(argv[i], "--churn") == 0) {
      if (++i >= argc || !parse_size_arg(argv[i], &options->churn)) {
        fprintf(stderr, "--churn requires a non-negative integer\n");
        return false;
      }
    } else if (strcmp(argv[i], "--seed") == 0) {
      if (++i >= argc || !parse_uint_arg(argv[i], &options->seed)) {
        fprintf(stderr, "--seed requires an unsigned integer\n");
        return false;
      }
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      return false;
    }
  }

  return true;
}

static bool workload_selected(const BenchOptions* options, const char* name) {
  return strcmp(options->workload, "all") == 0 || strcmp(options->workload, name) == 0;
}

static void print_csv_header(void) {
  printf("workload,repeat,units,elapsed_seconds,alloc_count,approx_bytes,allocs_per_sec,mb_per_sec,minor_collections,full_collections,page_count,free_pages,nursery_pages,survivor_pages,old_pages,large_pages,remembered_slots,live_bytes,checksum\n");
}

static void print_human_header(void) {
  printf("%-11s %6s %10s %10s %12s %12s %8s %6s %6s %6s %6s %6s %6s %6s %6s %9s %11s\n",
      "workload",
      "repeat",
      "seconds",
      "allocs",
      "bytes",
      "alloc/s",
      "MB/s",
      "minor",
      "full",
      "pages",
      "free",
      "nursery",
      "surv",
      "old",
      "large",
      "remember",
      "live_bytes");
}

static void print_report(const BenchOptions* options, const BenchReport* report) {
  const double elapsed = report->elapsed_seconds > 0.0 ? report->elapsed_seconds : 0.000000001;
  const double allocs_per_sec = (double) report->alloc_count / elapsed;
  const double mb_per_sec = ((double) report->approx_bytes / (1024.0 * 1024.0)) / elapsed;

  if (options->csv) {
    printf("%s,%zu,%zu,%.9f,%zu,%zu,%.3f,%.3f,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu\n",
        report->workload,
        report->repeat_index,
        report->units,
        report->elapsed_seconds,
        report->alloc_count,
        report->approx_bytes,
        allocs_per_sec,
        mb_per_sec,
        report->minor_collections,
        report->full_collections,
        report->page_count,
        report->pages.free_pages,
        report->pages.nursery_pages,
        report->pages.survivor_pages,
        report->pages.old_pages,
        report->pages.large_pages,
        report->remembered_slots,
        report->live_bytes,
        report->checksum);
  } else {
    printf("%-11s %6zu %10.6f %10zu %12zu %12.0f %8.1f %6zu %6zu %6zu %6zu %6zu %6zu %6zu %6zu %9zu %11zu\n",
        report->workload,
        report->repeat_index,
        report->elapsed_seconds,
        report->alloc_count,
        report->approx_bytes,
        allocs_per_sec,
        mb_per_sec,
        report->minor_collections,
        report->full_collections,
        report->page_count,
        report->pages.free_pages,
        report->pages.nursery_pages,
        report->pages.survivor_pages,
        report->pages.old_pages,
        report->pages.large_pages,
        report->remembered_slots,
        report->live_bytes);
  }
}

int main(int argc, char** argv) {
  BenchOptions options;
  bool ran_any = false;
  bool ok = true;

  if (!parse_options(argc, argv, &options)) {
    print_usage(stderr, argv[0]);
    return 2;
  }

  if (options.csv) {
    print_csv_header();
  } else {
    print_human_header();
  }

  for (size_t i = 0; i < ARRAY_LEN(workloads); i++) {
    const BenchWorkload* workload = &workloads[i];

    if (!workload_selected(&options, workload->name)) {
      continue;
    }

    ran_any = true;
    for (size_t repeat_index = 0; repeat_index < options.repeat; repeat_index++) {
      BenchReport report;

      if (!workload->run(&options, repeat_index, &report)) {
        fprintf(stderr, "workload failed: %s repeat %zu\n", workload->name, repeat_index);
        ok = false;
        break;
      }

      print_report(&options, &report);
    }
  }

  if (!ran_any) {
    fprintf(stderr, "unknown workload: %s\n", options.workload);
    return 2;
  }

  return ok ? 0 : 1;
}

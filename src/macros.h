#ifndef MACROS_H
#define MACROS_H

#define ARRAY_LEN(x) (sizeof(x) / sizeof(x)[0])
#define KB(x) ((size_t) (x) * 1024ull)
#define MB(x) ((size_t) (x) * 1024ull * 1024ull)
#define GB(x) ((size_t) (x) * 1024ull * 1024ull * 1024ull)

#define ALIGN_UP(x, a) \
  ((((size_t) (x)) + (((size_t) (a)) - 1u)) & ~(((size_t) (a)) - 1u))
#define ALIGN_DOWN(x, a) \
  (((size_t) (x)) & ~(((size_t) (a)) - 1u))

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#endif

#ifndef MACROS_H
#define MACROS_H

#define ARRAY_LEN(x) (sizeof(x) / sizeof(x)[0])
#define KB(x) ((size_t) (x) * 1024ull)
#define MB(x) ((size_t) (x) * 1024ull * 1024ull)
#define GB(x) ((size_t) (x) * 1024ull * 1024ull * 1024ull)

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#endif

#ifndef PLATFORM_H
#define PLATFORM_H

#if defined(__x86_64__) || defined(_M_X64)
  #define GC_ARCH_X64 1
#else
  #define GC_ARCH_X64 0
#endif

#if defined(__linux__)
  #define GC_OS_LINUX 1
#else
  #define GC_OS_LINUX 0
#endif

#if defined(__APPLE__)
  #define GC_OS_MAC 1
#else
  #define GC_OS_MAC 0
#endif

#endif

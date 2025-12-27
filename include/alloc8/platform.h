// alloc8/platform.h - Platform detection and compiler abstractions
#pragma once

// ─── PLATFORM DETECTION ───────────────────────────────────────────────────────

#if defined(__linux__)
  #define ALLOC8_LINUX 1
  #define ALLOC8_POSIX 1
#elif defined(__APPLE__) && defined(__MACH__)
  #define ALLOC8_MACOS 1
  #define ALLOC8_POSIX 1
#elif defined(_WIN32) || defined(_WIN64)
  #define ALLOC8_WINDOWS 1
#else
  #error "Unsupported platform"
#endif

// ─── ARCHITECTURE DETECTION ───────────────────────────────────────────────────

#if defined(__x86_64__) || defined(_M_X64)
  #define ALLOC8_ARCH_X64 1
#elif defined(__i386__) || defined(_M_IX86)
  #define ALLOC8_ARCH_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64)
  #define ALLOC8_ARCH_ARM64 1
#elif defined(__arm__) || defined(_M_ARM)
  #define ALLOC8_ARCH_ARM 1
#endif

// ─── COMPILER DETECTION ───────────────────────────────────────────────────────

#if defined(__clang__)
  #define ALLOC8_CLANG 1
#elif defined(__GNUC__)
  #define ALLOC8_GCC 1
#elif defined(_MSC_VER)
  #define ALLOC8_MSVC 1
#endif

// ─── VISIBILITY / EXPORT MACROS ───────────────────────────────────────────────

#if defined(ALLOC8_WINDOWS)
  #define ALLOC8_EXPORT __declspec(dllexport)
  #define ALLOC8_IMPORT __declspec(dllimport)
  #define ALLOC8_HIDDEN
#else
  #define ALLOC8_EXPORT __attribute__((visibility("default")))
  #define ALLOC8_IMPORT
  #define ALLOC8_HIDDEN __attribute__((visibility("hidden")))
#endif

// ─── FUNCTION ATTRIBUTES ──────────────────────────────────────────────────────

#if defined(ALLOC8_MSVC)
  #define ALLOC8_ALWAYS_INLINE __forceinline
  #define ALLOC8_NOINLINE __declspec(noinline)
  #define ALLOC8_CDECL __cdecl
  #define ALLOC8_MALLOC_ATTR
  #define ALLOC8_ALLOC_SIZE(...)
  #define ALLOC8_FLATTEN
#else
  #define ALLOC8_ALWAYS_INLINE __attribute__((always_inline)) inline
  #define ALLOC8_NOINLINE __attribute__((noinline))
  #define ALLOC8_CDECL
  #define ALLOC8_MALLOC_ATTR __attribute__((malloc))
  #define ALLOC8_ALLOC_SIZE(...) __attribute__((alloc_size(__VA_ARGS__)))
  #define ALLOC8_FLATTEN __attribute__((flatten))
#endif

// ─── LIKELY/UNLIKELY HINTS ────────────────────────────────────────────────────
// Use __builtin_expect for portability (C++20 [[likely]]/[[unlikely]] have
// different syntax requirements that make macro usage difficult)

#if defined(ALLOC8_GCC) || defined(ALLOC8_CLANG)
  #define ALLOC8_LIKELY(x)   __builtin_expect(!!(x), 1)
  #define ALLOC8_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
  #define ALLOC8_LIKELY(x)   (x)
  #define ALLOC8_UNLIKELY(x) (x)
#endif

// ─── ALIAS MACROS (GCC/Clang only) ────────────────────────────────────────────

#if defined(ALLOC8_GCC) || defined(ALLOC8_CLANG)
  #define ALLOC8_STRONG_ALIAS(target) \
    __attribute__((alias(#target), visibility("default")))
  #define ALLOC8_WEAK_ALIAS(target) \
    __attribute__((weak, alias(#target), visibility("default")))
#endif

// ─── THROW SPECIFICATION ──────────────────────────────────────────────────────

#if defined(__cplusplus)
  #if __cplusplus >= 201103L
    #define ALLOC8_THROW noexcept(false)
    #define ALLOC8_NOTHROW noexcept
  #else
    #define ALLOC8_THROW throw()
    #define ALLOC8_NOTHROW throw()
  #endif
#else
  #define ALLOC8_THROW
  #define ALLOC8_NOTHROW
#endif

// ─── PAGE SIZE ────────────────────────────────────────────────────────────────

#if defined(ALLOC8_WINDOWS)
  #define ALLOC8_PAGE_SIZE 4096
#elif defined(__APPLE__) && defined(ALLOC8_ARCH_ARM64)
  #define ALLOC8_PAGE_SIZE 16384
#else
  #define ALLOC8_PAGE_SIZE 4096
#endif

// ─── ALIGNMENT ────────────────────────────────────────────────────────────────

#define ALLOC8_MIN_ALIGNMENT 16

// ─── CACHE LINE SIZE ──────────────────────────────────────────────────────────

#if defined(ALLOC8_ARCH_ARM64) && defined(__APPLE__)
  #define ALLOC8_CACHE_LINE_SIZE 128
#else
  #define ALLOC8_CACHE_LINE_SIZE 64
#endif

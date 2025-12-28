// alloc8/include/alloc8/gnu_wrapper.h
// Header-only Linux wrapper for zero-overhead interposition
//
// Usage: Define getCustomHeap() returning your heap singleton, then include this header.
//
// Example:
//   class TheCustomHeapType : public MyHeap {};
//
//   inline static TheCustomHeapType* getCustomHeap() {
//     static char buf[sizeof(TheCustomHeapType)];
//     static TheCustomHeapType* heap = new (buf) TheCustomHeapType;
//     return heap;
//   }
//
//   #include <alloc8/gnu_wrapper.h>
//
// The heap type must provide:
//   void* malloc(size_t sz)
//   void free(void* ptr)
//   void* memalign(size_t alignment, size_t sz)  // or just return malloc(max(alignment, sz))
//   size_t getSize(void* ptr)
//   void lock()    // for fork safety
//   void unlock()  // for fork safety

#pragma once

#ifndef __GNUC__
#error "This file requires GCC or Clang"
#endif

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <malloc.h>
#include <unistd.h>
#include <pthread.h>
#include <limits.h>
#include <new>

#include "platform.h"

// ─── HELPER MACROS ──────────────────────────────────────────────────────────

#define ALLOC8_WRAPPER_EXPORT __attribute__((visibility("default")))

#ifndef __THROW
#define __THROW
#endif

// ─── INTERNAL INLINE HELPERS ────────────────────────────────────────────────
// These call getCustomHeap() directly for maximum inlining with LTO

namespace alloc8_internal {
  inline void* do_malloc(size_t sz) {
    return getCustomHeap()->malloc(sz);
  }

  inline void do_free(void* ptr) {
    getCustomHeap()->free(ptr);
  }

  inline void* do_memalign(size_t alignment, size_t sz) {
    return getCustomHeap()->memalign(alignment, sz);
  }

  inline size_t do_getsize(void* ptr) {
    return getCustomHeap()->getSize(ptr);
  }
}

// ─── CORE ALLOCATION FUNCTIONS ───────────────────────────────────────────────

extern "C" ALLOC8_WRAPPER_EXPORT void* malloc(size_t sz) __THROW {
  return alloc8_internal::do_malloc(sz);
}

extern "C" ALLOC8_WRAPPER_EXPORT void free(void* ptr) __THROW {
  if (ALLOC8_LIKELY(ptr != nullptr)) {
    alloc8_internal::do_free(ptr);
  }
}

extern "C" ALLOC8_WRAPPER_EXPORT void* calloc(size_t nelem, size_t elsize) __THROW {
  size_t total = nelem * elsize;
  if (ALLOC8_UNLIKELY(elsize != 0 && total / elsize != nelem)) {
    return nullptr;
  }
  void* ptr = alloc8_internal::do_malloc(total);
  if (ALLOC8_LIKELY(ptr != nullptr)) {
    memset(ptr, 0, total);
  }
  return ptr;
}

extern "C" ALLOC8_WRAPPER_EXPORT void* realloc(void* ptr, size_t sz) __THROW {
  if (!ptr) {
    return alloc8_internal::do_malloc(sz);
  }
  if (sz == 0) {
    alloc8_internal::do_free(ptr);
    return nullptr;
  }
  size_t oldSize = alloc8_internal::do_getsize(ptr);
  void* newPtr = alloc8_internal::do_malloc(sz);
  if (newPtr) {
    size_t copySize = (oldSize < sz) ? oldSize : sz;
    memcpy(newPtr, ptr, copySize);
    alloc8_internal::do_free(ptr);
  }
  return newPtr;
}

extern "C" ALLOC8_WRAPPER_EXPORT void* reallocarray(void* ptr, size_t nmemb, size_t size) __THROW {
  if (ALLOC8_UNLIKELY(size != 0 && nmemb > SIZE_MAX / size)) {
    errno = ENOMEM;
    return nullptr;
  }
  return realloc(ptr, nmemb * size);
}

extern "C" ALLOC8_WRAPPER_EXPORT void* memalign(size_t alignment, size_t size) __THROW {
  return alloc8_internal::do_memalign(alignment, size);
}

extern "C" ALLOC8_WRAPPER_EXPORT int posix_memalign(void** memptr, size_t alignment, size_t size) __THROW {
  *memptr = nullptr;
  if (ALLOC8_UNLIKELY(alignment == 0 ||
      (alignment % sizeof(void*)) != 0 ||
      (alignment & (alignment - 1)) != 0)) {
    return EINVAL;
  }
  void* ptr = alloc8_internal::do_memalign(alignment, size);
  if (ALLOC8_UNLIKELY(!ptr)) {
    return ENOMEM;
  }
  *memptr = ptr;
  return 0;
}

extern "C" ALLOC8_WRAPPER_EXPORT void* aligned_alloc(size_t alignment, size_t size) __THROW {
  if (alignment == 0 || (size % alignment) != 0) {
    return nullptr;
  }
  return alloc8_internal::do_memalign(alignment, size);
}

extern "C" ALLOC8_WRAPPER_EXPORT size_t malloc_usable_size(void* ptr) __THROW {
  return alloc8_internal::do_getsize(ptr);
}

extern "C" ALLOC8_WRAPPER_EXPORT void cfree(void* ptr) __THROW {
  if (ALLOC8_LIKELY(ptr != nullptr)) {
    alloc8_internal::do_free(ptr);
  }
}

// ─── STRING FUNCTIONS ────────────────────────────────────────────────────────

extern "C" ALLOC8_WRAPPER_EXPORT char* strdup(const char* s) __THROW {
  if (!s) return nullptr;
  size_t len = strlen(s) + 1;
  char* newStr = (char*)alloc8_internal::do_malloc(len);
  if (newStr) {
    memcpy(newStr, s, len);
  }
  return newStr;
}

extern "C" ALLOC8_WRAPPER_EXPORT char* strndup(const char* s, size_t n) __THROW {
  if (!s) return nullptr;
  size_t len = strnlen(s, n);
  char* newStr = (char*)alloc8_internal::do_malloc(len + 1);
  if (newStr) {
    memcpy(newStr, s, len);
    newStr[len] = '\0';
  }
  return newStr;
}

// ─── PAGE-ALIGNED ALLOCATION ─────────────────────────────────────────────────

extern "C" ALLOC8_WRAPPER_EXPORT void* valloc(size_t sz) __THROW {
  return alloc8_internal::do_memalign(ALLOC8_PAGE_SIZE, sz);
}

extern "C" ALLOC8_WRAPPER_EXPORT void* pvalloc(size_t sz) __THROW {
  size_t pagesize = ALLOC8_PAGE_SIZE;
  size_t rounded = (sz + pagesize - 1) & ~(pagesize - 1);
  return alloc8_internal::do_memalign(pagesize, rounded);
}

// ─── GNU EXTENSIONS (STUBS) ──────────────────────────────────────────────────

extern "C" ALLOC8_WRAPPER_EXPORT int mallopt(int, int) __THROW {
  return 1;
}

extern "C" ALLOC8_WRAPPER_EXPORT int malloc_trim(size_t) __THROW {
  return 0;
}

extern "C" ALLOC8_WRAPPER_EXPORT void malloc_stats() __THROW {
}

#if defined(__GLIBC__)
extern "C" ALLOC8_WRAPPER_EXPORT struct mallinfo mallinfo() __THROW {
  struct mallinfo m = {};
  return m;
}
#endif

// ─── GLIBC __libc_* SYMBOLS ──────────────────────────────────────────────────

#if defined(__GLIBC__)
extern "C" ALLOC8_WRAPPER_EXPORT void* __libc_malloc(size_t n) __THROW {
  return alloc8_internal::do_malloc(n);
}
extern "C" ALLOC8_WRAPPER_EXPORT void __libc_free(void* p) __THROW {
  if (p) alloc8_internal::do_free(p);
}
extern "C" ALLOC8_WRAPPER_EXPORT void* __libc_calloc(size_t a, size_t b) __THROW {
  return calloc(a, b);
}
extern "C" ALLOC8_WRAPPER_EXPORT void* __libc_realloc(void* p, size_t n) __THROW {
  return realloc(p, n);
}
extern "C" ALLOC8_WRAPPER_EXPORT void* __libc_memalign(size_t m, size_t n) __THROW {
  return alloc8_internal::do_memalign(m, n);
}
#endif

// ─── FORK SAFETY ─────────────────────────────────────────────────────────────

namespace {
  static void alloc8_fork_prepare() { getCustomHeap()->lock(); }
  static void alloc8_fork_parent()  { getCustomHeap()->unlock(); }
  static void alloc8_fork_child()   { getCustomHeap()->unlock(); }

  __attribute__((constructor))
  static void alloc8_register_fork_handlers() {
    pthread_atfork(alloc8_fork_prepare, alloc8_fork_parent, alloc8_fork_child);
  }
}

// ─── C++ OPERATOR NEW/DELETE ─────────────────────────────────────────────────

void* operator new(size_t sz) {
  void* ptr = alloc8_internal::do_malloc(sz);
  if (ALLOC8_UNLIKELY(ptr == nullptr && sz != 0)) {
    throw std::bad_alloc();
  }
  return ptr;
}

void* operator new[](size_t sz) {
  void* ptr = alloc8_internal::do_malloc(sz);
  if (ALLOC8_UNLIKELY(ptr == nullptr && sz != 0)) {
    throw std::bad_alloc();
  }
  return ptr;
}

void* operator new(size_t sz, const std::nothrow_t&) noexcept {
  return alloc8_internal::do_malloc(sz);
}

void* operator new[](size_t sz, const std::nothrow_t&) noexcept {
  return alloc8_internal::do_malloc(sz);
}

void operator delete(void* ptr) noexcept {
  if (ptr) alloc8_internal::do_free(ptr);
}

void operator delete[](void* ptr) noexcept {
  if (ptr) alloc8_internal::do_free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
  if (ptr) alloc8_internal::do_free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  if (ptr) alloc8_internal::do_free(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
  if (ptr) alloc8_internal::do_free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
  if (ptr) alloc8_internal::do_free(ptr);
}

// C++17 aligned new/delete
void* operator new(size_t sz, std::align_val_t align) {
  void* ptr = alloc8_internal::do_memalign(static_cast<size_t>(align), sz);
  if (ALLOC8_UNLIKELY(ptr == nullptr && sz != 0)) {
    throw std::bad_alloc();
  }
  return ptr;
}

void* operator new[](size_t sz, std::align_val_t align) {
  void* ptr = alloc8_internal::do_memalign(static_cast<size_t>(align), sz);
  if (ALLOC8_UNLIKELY(ptr == nullptr && sz != 0)) {
    throw std::bad_alloc();
  }
  return ptr;
}

void* operator new(size_t sz, std::align_val_t align, const std::nothrow_t&) noexcept {
  return alloc8_internal::do_memalign(static_cast<size_t>(align), sz);
}

void* operator new[](size_t sz, std::align_val_t align, const std::nothrow_t&) noexcept {
  return alloc8_internal::do_memalign(static_cast<size_t>(align), sz);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
  if (ptr) alloc8_internal::do_free(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
  if (ptr) alloc8_internal::do_free(ptr);
}

void operator delete(void* ptr, size_t, std::align_val_t) noexcept {
  if (ptr) alloc8_internal::do_free(ptr);
}

void operator delete[](void* ptr, size_t, std::align_val_t) noexcept {
  if (ptr) alloc8_internal::do_free(ptr);
}

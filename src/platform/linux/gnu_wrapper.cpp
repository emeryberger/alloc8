// alloc8/src/platform/linux/gnu_wrapper.cpp
// Linux allocator interposition via strong symbol aliasing
//
// Reference: Heap-Layers gnuwrapper.cpp by Emery Berger

#ifndef __GNUC__
#error "This file requires GCC or Clang"
#endif

#include <alloc8/alloc8.h>

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <limits.h>
#include <new>

// ─── FORWARD DECLARATIONS ─────────────────────────────────────────────────────
// These are provided by the user via ALLOC8_REDIRECT macro

extern "C" {
  void* xxmalloc(size_t);
  void  xxfree(void*);
  void* xxmemalign(size_t, size_t);
  size_t xxmalloc_usable_size(void*);
  void xxmalloc_lock();
  void xxmalloc_unlock();
  void* xxrealloc(void*, size_t);
  void* xxcalloc(size_t, size_t);
}

// ─── INTERNAL PREFIX ──────────────────────────────────────────────────────────

#define CUSTOM_PREFIX(x) custom##x

#ifndef __THROW
#define __THROW
#endif

// ─── ALIAS MACROS ─────────────────────────────────────────────────────────────

#define ATTRIBUTE_EXPORT __attribute__((visibility("default")))
#define STRONG_ALIAS(target) __attribute__((alias(#target), visibility("default")))

#define STRONG_REDEF1(type, fname, arg1) \
  ATTRIBUTE_EXPORT type fname(arg1) __THROW STRONG_ALIAS(custom##fname)

#define STRONG_REDEF2(type, fname, arg1, arg2) \
  ATTRIBUTE_EXPORT type fname(arg1, arg2) __THROW STRONG_ALIAS(custom##fname)

#define STRONG_REDEF3(type, fname, arg1, arg2, arg3) \
  ATTRIBUTE_EXPORT type fname(arg1, arg2, arg3) __THROW STRONG_ALIAS(custom##fname)

// ─── THREAD-LOCAL FOR DLSYM RECURSION ─────────────────────────────────────────

static __thread int in_dlsym = 0;

__attribute__((noinline))
static void* safe_dlsym(void* handle, const char* symbol) {
  ++in_dlsym;
  void* ptr = dlsym(handle, symbol);
  --in_dlsym;
  return ptr;
}

// ─── CORE ALLOCATION FUNCTIONS ────────────────────────────────────────────────

extern "C" ATTRIBUTE_EXPORT __attribute__((flatten))
void* CUSTOM_PREFIX(malloc)(size_t sz) {
  return xxmalloc(sz);
}

extern "C" ATTRIBUTE_EXPORT __attribute__((flatten))
void CUSTOM_PREFIX(free)(void* ptr) {
  if (ALLOC8_LIKELY(ptr != nullptr)) {
    xxfree(ptr);
  }
}

extern "C" ATTRIBUTE_EXPORT __attribute__((flatten))
void* CUSTOM_PREFIX(calloc)(size_t nelem, size_t elsize) {
  // Reject calls from dlsym to avoid recursion
  if (ALLOC8_UNLIKELY(in_dlsym)) {
    return nullptr;
  }
  return xxcalloc(nelem, elsize);
}

extern "C" ATTRIBUTE_EXPORT __attribute__((flatten))
void* CUSTOM_PREFIX(realloc)(void* ptr, size_t sz) {
  return xxrealloc(ptr, sz);
}

extern "C" ATTRIBUTE_EXPORT __attribute__((flatten))
void* CUSTOM_PREFIX(reallocarray)(void* ptr, size_t nmemb, size_t size) {
  // Overflow check
  if (ALLOC8_UNLIKELY(size != 0 && nmemb > SIZE_MAX / size)) {
    errno = ENOMEM;
    return nullptr;
  }
  return xxrealloc(ptr, nmemb * size);
}

extern "C" ATTRIBUTE_EXPORT __attribute__((flatten))
void* CUSTOM_PREFIX(memalign)(size_t alignment, size_t size) __THROW {
  return xxmemalign(alignment, size);
}

extern "C" ATTRIBUTE_EXPORT __attribute__((flatten))
int CUSTOM_PREFIX(posix_memalign)(void** memptr, size_t alignment, size_t size) __THROW {
  *memptr = nullptr;

  // Alignment must be power of 2 and multiple of sizeof(void*)
  if (ALLOC8_UNLIKELY(alignment == 0 ||
      (alignment % sizeof(void*)) != 0 ||
      (alignment & (alignment - 1)) != 0)) {
    return EINVAL;
  }

  void* ptr = xxmemalign(alignment, size);
  if (ALLOC8_UNLIKELY(!ptr)) {
    return ENOMEM;
  }

  *memptr = ptr;
  return 0;
}

extern "C" ATTRIBUTE_EXPORT __attribute__((flatten))
void* CUSTOM_PREFIX(aligned_alloc)(size_t alignment, size_t size) __THROW {
  // C11: size must be multiple of alignment
  if (alignment == 0 || (size % alignment) != 0) {
    return nullptr;
  }
  return xxmemalign(alignment, size);
}

extern "C" ATTRIBUTE_EXPORT __attribute__((flatten))
size_t CUSTOM_PREFIX(malloc_usable_size)(void* ptr) {
  return xxmalloc_usable_size(ptr);
}

// Legacy cfree
extern "C" ATTRIBUTE_EXPORT
void CUSTOM_PREFIX(cfree)(void* ptr) {
  if (ALLOC8_LIKELY(ptr != nullptr)) {
    xxfree(ptr);
  }
}

// ─── STRING FUNCTIONS ─────────────────────────────────────────────────────────

extern "C" ATTRIBUTE_EXPORT
char* CUSTOM_PREFIX(strdup)(const char* s) {
  if (!s) return nullptr;
  size_t len = strlen(s) + 1;
  char* newStr = (char*)xxmalloc(len);
  if (newStr) {
    memcpy(newStr, s, len);
  }
  return newStr;
}

extern "C" ATTRIBUTE_EXPORT
char* CUSTOM_PREFIX(strndup)(const char* s, size_t n) {
  if (!s) return nullptr;
  size_t len = strnlen(s, n);
  char* newStr = (char*)xxmalloc(len + 1);
  if (newStr) {
    memcpy(newStr, s, len);
    newStr[len] = '\0';
  }
  return newStr;
}

// ─── PAGE-ALIGNED ALLOCATION ──────────────────────────────────────────────────

extern "C" ATTRIBUTE_EXPORT
void* CUSTOM_PREFIX(valloc)(size_t sz) {
  return xxmemalign(ALLOC8_PAGE_SIZE, sz);
}

extern "C" ATTRIBUTE_EXPORT
void* CUSTOM_PREFIX(pvalloc)(size_t sz) {
  // Round up to page size
  size_t pagesize = ALLOC8_PAGE_SIZE;
  size_t rounded = (sz + pagesize - 1) & ~(pagesize - 1);
  return xxmemalign(pagesize, rounded);
}

// ─── GNU EXTENSIONS (STUBS) ───────────────────────────────────────────────────

extern "C" ATTRIBUTE_EXPORT
int CUSTOM_PREFIX(mallopt)(int /* param */, int /* value */) {
  return 1; // success (NOP)
}

extern "C" ATTRIBUTE_EXPORT
int CUSTOM_PREFIX(malloc_trim)(size_t /* pad */) {
  return 0; // no memory released
}

extern "C" ATTRIBUTE_EXPORT
void CUSTOM_PREFIX(malloc_stats)() {
  // NOP
}

#if defined(__GLIBC__)
extern "C" ATTRIBUTE_EXPORT
struct mallinfo CUSTOM_PREFIX(mallinfo)() {
  struct mallinfo m = {};
  return m;
}
#endif

// ─── GETCWD WRAPPER ───────────────────────────────────────────────────────────

typedef char* (*getcwd_fn)(char*, size_t);

extern "C" ATTRIBUTE_EXPORT
char* CUSTOM_PREFIX(getcwd)(char* buf, size_t size) {
  static getcwd_fn real_getcwd = nullptr;
  if (!real_getcwd) {
    real_getcwd = (getcwd_fn)(uintptr_t)safe_dlsym(RTLD_NEXT, "getcwd");
  }

  if (!buf) {
    if (size == 0) {
      size = PATH_MAX;
    }
    buf = (char*)xxmalloc(size);
  }
  return real_getcwd(buf, size);
}

// ─── STRONG SYMBOL ALIASES ────────────────────────────────────────────────────
// These create the actual malloc/free symbols that override libc

extern "C" {
  STRONG_REDEF1(void*, malloc, size_t);
  STRONG_REDEF1(void, free, void*);
  STRONG_REDEF1(void, cfree, void*);
  STRONG_REDEF2(void*, calloc, size_t, size_t);
  STRONG_REDEF2(void*, realloc, void*, size_t);
  STRONG_REDEF3(void*, reallocarray, void*, size_t, size_t);
  STRONG_REDEF2(void*, memalign, size_t, size_t);
  STRONG_REDEF3(int, posix_memalign, void**, size_t, size_t);
  STRONG_REDEF2(void*, aligned_alloc, size_t, size_t);
  STRONG_REDEF1(size_t, malloc_usable_size, void*);
  STRONG_REDEF1(char*, strdup, const char*);
  STRONG_REDEF2(char*, strndup, const char*, size_t);
  STRONG_REDEF1(void*, valloc, size_t);
  STRONG_REDEF1(void*, pvalloc, size_t);
}

// ─── GLIBC __libc_* SYMBOLS ───────────────────────────────────────────────────
// Some programs call these directly

#if defined(__GLIBC__)
extern "C" {
  ATTRIBUTE_EXPORT void* __libc_malloc(size_t n) { return xxmalloc(n); }
  ATTRIBUTE_EXPORT void  __libc_free(void* p) { if (p) xxfree(p); }
  ATTRIBUTE_EXPORT void* __libc_calloc(size_t a, size_t b) { return xxcalloc(a, b); }
  ATTRIBUTE_EXPORT void* __libc_realloc(void* p, size_t n) { return xxrealloc(p, n); }
  ATTRIBUTE_EXPORT void* __libc_memalign(size_t m, size_t n) { return xxmemalign(m, n); }
}
#endif

// ─── FORK SAFETY ──────────────────────────────────────────────────────────────

static void fork_prepare() { xxmalloc_lock(); }
static void fork_parent()  { xxmalloc_unlock(); }
static void fork_child()   { xxmalloc_unlock(); }

__attribute__((constructor))
static void register_fork_handlers() {
  pthread_atfork(fork_prepare, fork_parent, fork_child);
}

// ─── C++ OPERATOR NEW/DELETE ──────────────────────────────────────────────────
// Include from common (separate file to share with macOS)

#include "../../common/new_delete.inc"

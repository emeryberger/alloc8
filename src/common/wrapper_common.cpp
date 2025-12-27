// alloc8/src/common/wrapper_common.cpp
// Common wrapper implementations shared across platforms

#include <alloc8/alloc8.h>
#include <cstring>
#include <cstdlib>
#include <cerrno>

// Forward declarations
extern "C" {
  void* xxmalloc(size_t);
  void  xxfree(void*);
  size_t xxmalloc_usable_size(void*);
  void* xxmemalign(size_t, size_t);
  void xxmalloc_lock();
  void xxmalloc_unlock();
}

// ─── HELPER FUNCTIONS ─────────────────────────────────────────────────────────

namespace alloc8 {
namespace internal {

// Get page size (platform-specific)
inline size_t getPageSize() {
#if defined(_WIN32)
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwPageSize;
#else
  return ALLOC8_PAGE_SIZE;
#endif
}

} // namespace internal
} // namespace alloc8

// ─── COMMON ALLOCATION WRAPPERS ───────────────────────────────────────────────
// These can be used by platform wrappers if they don't implement their own

extern "C" {

/**
 * calloc implementation with overflow check.
 */
ALLOC8_EXPORT void* alloc8_common_calloc(size_t count, size_t size) {
  // Overflow check
  size_t total = count * size;
  if (ALLOC8_UNLIKELY(size != 0 && total / size != count)) {
    return nullptr;
  }

  void* ptr = xxmalloc(total);
  if (ALLOC8_LIKELY(ptr != nullptr)) {
    memset(ptr, 0, total);
  }
  return ptr;
}

/**
 * realloc implementation.
 */
ALLOC8_EXPORT void* alloc8_common_realloc(void* ptr, size_t sz) {
  if (!ptr) {
    return xxmalloc(sz);
  }

  if (sz == 0) {
    xxfree(ptr);
#if defined(__APPLE__)
    // macOS: return small allocation on size 0
    return xxmalloc(1);
#else
    return nullptr;
#endif
  }

  size_t oldSize = xxmalloc_usable_size(ptr);

  // Don't reallocate if shrinking by less than half
  if ((oldSize / 2 < sz) && (sz <= oldSize)) {
    return ptr;
  }

  void* newPtr = xxmalloc(sz);
  if (ALLOC8_UNLIKELY(!newPtr)) {
    return nullptr;  // Keep original on failure
  }

  size_t copySize = (oldSize < sz) ? oldSize : sz;
  memcpy(newPtr, ptr, copySize);
  xxfree(ptr);

  return newPtr;
}

/**
 * posix_memalign implementation.
 */
ALLOC8_EXPORT int alloc8_common_posix_memalign(void** memptr, size_t alignment, size_t size) {
  *memptr = nullptr;

  // Alignment must be power of 2 and multiple of sizeof(void*)
  if (alignment < sizeof(void*) ||
      (alignment & (alignment - 1)) != 0) {
    return EINVAL;
  }

  void* ptr = xxmemalign(alignment, size);
  if (!ptr && size != 0) {
    return ENOMEM;
  }

  *memptr = ptr;
  return 0;
}

/**
 * aligned_alloc (C11) implementation.
 */
ALLOC8_EXPORT void* alloc8_common_aligned_alloc(size_t alignment, size_t size) {
  // C11: size must be multiple of alignment
  if (alignment == 0 || (size % alignment) != 0) {
    return nullptr;
  }
  return xxmemalign(alignment, size);
}

/**
 * valloc implementation (page-aligned).
 */
ALLOC8_EXPORT void* alloc8_common_valloc(size_t sz) {
  return xxmemalign(alloc8::internal::getPageSize(), sz);
}

/**
 * pvalloc implementation (page-aligned, size rounded to page).
 */
ALLOC8_EXPORT void* alloc8_common_pvalloc(size_t sz) {
  size_t pageSize = alloc8::internal::getPageSize();
  size_t rounded = (sz + pageSize - 1) & ~(pageSize - 1);
  return xxmemalign(pageSize, rounded);
}

/**
 * strdup implementation.
 */
ALLOC8_EXPORT char* alloc8_common_strdup(const char* s) {
  if (!s) return nullptr;
  size_t len = strlen(s) + 1;
  char* newStr = (char*)xxmalloc(len);
  if (newStr) {
    memcpy(newStr, s, len);
  }
  return newStr;
}

/**
 * strndup implementation.
 */
ALLOC8_EXPORT char* alloc8_common_strndup(const char* s, size_t n) {
  if (!s) return nullptr;

  // Find actual length (up to n)
  size_t len = 0;
  while (len < n && s[len]) {
    ++len;
  }

  char* newStr = (char*)xxmalloc(len + 1);
  if (newStr) {
    memcpy(newStr, s, len);
    newStr[len] = '\0';
  }
  return newStr;
}

/**
 * reallocarray implementation (overflow-safe realloc).
 */
ALLOC8_EXPORT void* alloc8_common_reallocarray(void* ptr, size_t nmemb, size_t size) {
  // Overflow check
  if (ALLOC8_UNLIKELY(size != 0 && nmemb > SIZE_MAX / size)) {
    errno = ENOMEM;
    return nullptr;
  }
  return alloc8_common_realloc(ptr, nmemb * size);
}

} // extern "C"

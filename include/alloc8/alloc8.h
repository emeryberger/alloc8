// alloc8/alloc8.h - Main header for alloc8 allocator interposition library
#pragma once

#include "platform.h"
#include "allocator_traits.h"

// ─── XXMALLOC INTERFACE ───────────────────────────────────────────────────────
//
// The xxmalloc interface is the bridge between your custom allocator and the
// platform-specific wrappers. Your allocator provides the implementation via
// HeapRedirect<T>, and ALLOC8_REDIRECT generates these extern "C" functions.
//
// Platform wrappers then alias system malloc/free to call these functions.

/**
 * ALLOC8_REDIRECT: Generate the xxmalloc interface from a HeapRedirect type.
 *
 * This macro generates the extern "C" functions that platform wrappers use
 * to redirect system allocation calls to your custom allocator.
 *
 * Place this in exactly ONE .cpp file in your allocator library.
 *
 * Example:
 *   // my_allocator.cpp
 *   #include <alloc8/alloc8.h>
 *
 *   class MyHeap {
 *   public:
 *     void* malloc(size_t sz) { ... }
 *     void free(void* ptr) { ... }
 *     void* memalign(size_t align, size_t sz) { ... }
 *     size_t getSize(void* ptr) { ... }
 *     void lock() { ... }
 *     void unlock() { ... }
 *   };
 *
 *   using MyRedirect = alloc8::HeapRedirect<MyHeap>;
 *   ALLOC8_REDIRECT(MyRedirect);
 */
#define ALLOC8_REDIRECT(HeapRedirectType) \
  extern "C" { \
    ALLOC8_EXPORT void* xxmalloc(size_t sz) { \
      return HeapRedirectType::malloc(sz); \
    } \
    \
    ALLOC8_EXPORT void xxfree(void* ptr) { \
      HeapRedirectType::free(ptr); \
    } \
    \
    ALLOC8_EXPORT void* xxmemalign(size_t alignment, size_t sz) { \
      return HeapRedirectType::memalign(alignment, sz); \
    } \
    \
    ALLOC8_EXPORT size_t xxmalloc_usable_size(void* ptr) { \
      return HeapRedirectType::getSize(ptr); \
    } \
    \
    ALLOC8_EXPORT void xxmalloc_lock() { \
      HeapRedirectType::lock(); \
    } \
    \
    ALLOC8_EXPORT void xxmalloc_unlock() { \
      HeapRedirectType::unlock(); \
    } \
    \
    ALLOC8_EXPORT void* xxrealloc(void* ptr, size_t sz) { \
      return HeapRedirectType::realloc(ptr, sz); \
    } \
    \
    ALLOC8_EXPORT void* xxcalloc(size_t count, size_t sz) { \
      return HeapRedirectType::calloc(count, sz); \
    } \
  }

// ─── FORWARD DECLARATIONS ─────────────────────────────────────────────────────
//
// These are the functions your ALLOC8_REDIRECT generates.
// Platform wrappers include this header and call these.

extern "C" {
  void* xxmalloc(size_t sz);
  void  xxfree(void* ptr);
  void* xxmemalign(size_t alignment, size_t sz);
  size_t xxmalloc_usable_size(void* ptr);
  void xxmalloc_lock();
  void xxmalloc_unlock();
  void* xxrealloc(void* ptr, size_t sz);
  void* xxcalloc(size_t count, size_t sz);
}

// ─── USAGE INSTRUCTIONS ───────────────────────────────────────────────────────
//
// 1. Define your allocator class with the required methods:
//      - void* malloc(size_t sz)
//      - void free(void* ptr)
//      - void* memalign(size_t alignment, size_t sz)
//      - size_t getSize(void* ptr)
//      - void lock()
//      - void unlock()
//    Optional:
//      - void* realloc(void* ptr, size_t sz)  // if not provided, default used
//
// 2. Create a HeapRedirect type alias:
//      using MyRedirect = alloc8::HeapRedirect<MyAllocator>;
//
// 3. In ONE .cpp file, use ALLOC8_REDIRECT:
//      ALLOC8_REDIRECT(MyRedirect);
//
// 4. In CMakeLists.txt:
//      add_library(myalloc SHARED
//        my_allocator.cpp
//        ${ALLOC8_INTERPOSE_SOURCES}
//      )
//      target_link_libraries(myalloc PRIVATE alloc8::interpose)
//
// 5. Use with LD_PRELOAD (Linux), DYLD_INSERT_LIBRARIES (macOS), or
//    DLL injection (Windows).

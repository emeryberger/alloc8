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

// ─── THREAD REDIRECT MACRO ────────────────────────────────────────────────────
//
// For thread-aware allocators that need to track thread creation/destruction.
// Use this in addition to ALLOC8_REDIRECT for allocators with per-thread state.

/**
 * ALLOC8_THREAD_REDIRECT: Generate thread lifecycle hooks from a ThreadRedirect type.
 *
 * This macro generates the extern "C" functions that pthread interposition uses
 * to notify your allocator of thread creation and destruction.
 *
 * Place this in the same .cpp file as ALLOC8_REDIRECT.
 *
 * Example:
 *   class MyHeap {
 *   public:
 *     void* malloc(size_t sz) { ... }
 *     void free(void* ptr) { ... }
 *     // ... other required methods ...
 *
 *     // Thread hooks (optional)
 *     void threadInit() { ... }      // Initialize per-thread state
 *     void threadCleanup() { ... }   // Cleanup per-thread state
 *   };
 *
 *   using MyRedirect = alloc8::HeapRedirect<MyHeap>;
 *   using MyThreads = alloc8::ThreadRedirect<MyHeap>;
 *   ALLOC8_REDIRECT(MyRedirect);
 *   ALLOC8_THREAD_REDIRECT(MyThreads);
 *
 * Note: Link with ${ALLOC8_THREAD_SOURCES} to enable pthread interposition.
 */
#define ALLOC8_THREAD_REDIRECT(ThreadRedirectType) \
  extern "C" { \
    ALLOC8_EXPORT void xxthread_init(void) { \
      ThreadRedirectType::threadInit(); \
    } \
    \
    ALLOC8_EXPORT void xxthread_cleanup(void) { \
      ThreadRedirectType::threadCleanup(); \
    } \
  }

/**
 * ALLOC8_REDIRECT_WITH_THREADS: Combined heap + thread redirect.
 *
 * Use this for allocators that implement both heap operations and thread hooks.
 * Equivalent to calling both ALLOC8_REDIRECT and ALLOC8_THREAD_REDIRECT.
 *
 * Example:
 *   class MyThreadAwareHeap {
 *   public:
 *     // Heap operations
 *     void* malloc(size_t sz) { ... }
 *     void free(void* ptr) { ... }
 *     void* memalign(size_t align, size_t sz) { ... }
 *     size_t getSize(void* ptr) { ... }
 *     void lock() { ... }
 *     void unlock() { ... }
 *
 *     // Thread hooks
 *     void threadInit() { ... }
 *     void threadCleanup() { ... }
 *   };
 *
 *   using MyRedirect = alloc8::HeapRedirect<MyThreadAwareHeap>;
 *   ALLOC8_REDIRECT_WITH_THREADS(MyRedirect);
 */
#define ALLOC8_REDIRECT_WITH_THREADS(HeapRedirectType) \
  ALLOC8_REDIRECT(HeapRedirectType) \
  ALLOC8_THREAD_REDIRECT(alloc8::ThreadRedirect<typename HeapRedirectType::AllocatorType>)

// ─── FORWARD DECLARATIONS ─────────────────────────────────────────────────────
//
// These are the functions your ALLOC8_REDIRECT generates.
// Platform wrappers include this header and call these.

extern "C" {
  ALLOC8_EXPORT void* xxmalloc(size_t sz);
  ALLOC8_EXPORT void  xxfree(void* ptr);
  ALLOC8_EXPORT void* xxmemalign(size_t alignment, size_t sz);
  ALLOC8_EXPORT size_t xxmalloc_usable_size(void* ptr);
  ALLOC8_EXPORT void xxmalloc_lock();
  ALLOC8_EXPORT void xxmalloc_unlock();
  ALLOC8_EXPORT void* xxrealloc(void* ptr, size_t sz);
  ALLOC8_EXPORT void* xxcalloc(size_t count, size_t sz);

  // Thread hooks (optional - only if ALLOC8_THREAD_REDIRECT used)
  ALLOC8_EXPORT void xxthread_init(void);
  ALLOC8_EXPORT void xxthread_cleanup(void);
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
//      - void threadInit()      // called when new thread starts
//      - void threadCleanup()   // called when thread exits
//
// 2. Create a HeapRedirect type alias:
//      using MyRedirect = alloc8::HeapRedirect<MyAllocator>;
//
// 3. In ONE .cpp file, use ALLOC8_REDIRECT:
//      ALLOC8_REDIRECT(MyRedirect);
//
//    For thread-aware allocators, also add:
//      using MyThreads = alloc8::ThreadRedirect<MyAllocator>;
//      ALLOC8_THREAD_REDIRECT(MyThreads);
//
//    Or use the combined macro:
//      ALLOC8_REDIRECT_WITH_THREADS(MyRedirect);
//
// 4. In CMakeLists.txt:
//      add_library(myalloc SHARED
//        my_allocator.cpp
//        ${ALLOC8_INTERPOSE_SOURCES}
//        ${ALLOC8_THREAD_SOURCES}      # Add for thread hooks
//      )
//      target_link_libraries(myalloc PRIVATE alloc8::interpose)
//
// 5. Use with LD_PRELOAD (Linux), DYLD_INSERT_LIBRARIES (macOS), or
//    DLL injection (Windows).

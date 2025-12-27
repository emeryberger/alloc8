// alloc8/allocator_traits.h - C++ allocator concept and HeapRedirect template
#pragma once

#include "platform.h"
#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>

#if __cplusplus >= 202002L
#include <concepts>
#endif

namespace alloc8 {

// ─── ALLOCATOR CONCEPT (C++20) ────────────────────────────────────────────────

#if __cplusplus >= 202002L

/**
 * Concept defining the minimum interface for an allocator.
 * User allocators must satisfy this concept to work with alloc8.
 */
template<typename T>
concept Allocator = requires(T& allocator, void* ptr, size_t size, size_t alignment) {
  // Required: allocation
  { allocator.malloc(size) } -> std::convertible_to<void*>;

  // Required: deallocation
  { allocator.free(ptr) } -> std::same_as<void>;

  // Required: get usable size of allocation
  { allocator.getSize(ptr) } -> std::convertible_to<size_t>;

  // Required: aligned allocation
  { allocator.memalign(alignment, size) } -> std::convertible_to<void*>;

  // Required for fork safety
  { allocator.lock() } -> std::same_as<void>;
  { allocator.unlock() } -> std::same_as<void>;
};

/**
 * Optional extension: allocator provides native realloc.
 */
template<typename T>
concept AllocatorWithRealloc = Allocator<T> &&
  requires(T& allocator, void* ptr, size_t size) {
    { allocator.realloc(ptr, size) } -> std::convertible_to<void*>;
  };

#endif // C++20

// ─── HEAP REDIRECT TEMPLATE ───────────────────────────────────────────────────

/**
 * HeapRedirect: Bridges a user allocator to the xxmalloc interface.
 *
 * This template wraps a custom allocator class and provides static methods
 * that the platform wrappers expect. The allocator is instantiated as a
 * singleton that survives past atexit handlers (important for cleanup).
 *
 * @tparam AllocatorType Your custom allocator class (must satisfy Allocator concept)
 *
 * Usage:
 *   class MyHeap {
 *     void* malloc(size_t sz);
 *     void free(void* ptr);
 *     void* memalign(size_t align, size_t sz);
 *     size_t getSize(void* ptr);
 *     void lock();
 *     void unlock();
 *   };
 *
 *   using MyRedirect = alloc8::HeapRedirect<MyHeap>;
 *   ALLOC8_REDIRECT(MyRedirect);  // generates xxmalloc etc.
 */
template<typename AllocatorType>
class HeapRedirect {
public:
  /**
   * Get singleton heap instance.
   * Uses placement new into static buffer to ensure it survives past atexit.
   */
  ALLOC8_ALWAYS_INLINE
  static AllocatorType* getHeap() {
    alignas(AllocatorType) static char buffer[sizeof(AllocatorType)];
    static AllocatorType* heap = new (buffer) AllocatorType;
    return heap;
  }

  ALLOC8_ALWAYS_INLINE ALLOC8_MALLOC_ATTR ALLOC8_ALLOC_SIZE(1)
  static void* malloc(size_t sz) {
    return getHeap()->malloc(sz);
  }

  ALLOC8_ALWAYS_INLINE
  static void free(void* ptr) {
    if (ALLOC8_LIKELY(ptr != nullptr)) {
      getHeap()->free(ptr);
    }
  }

  ALLOC8_ALWAYS_INLINE ALLOC8_MALLOC_ATTR ALLOC8_ALLOC_SIZE(2)
  static void* memalign(size_t alignment, size_t sz) {
    return getHeap()->memalign(alignment, sz);
  }

  ALLOC8_ALWAYS_INLINE
  static size_t getSize(void* ptr) {
    return ptr ? getHeap()->getSize(ptr) : 0;
  }

  ALLOC8_ALWAYS_INLINE
  static void lock() {
    getHeap()->lock();
  }

  ALLOC8_ALWAYS_INLINE
  static void unlock() {
    getHeap()->unlock();
  }

  /**
   * Realloc with fallback implementation if allocator doesn't provide it.
   */
  ALLOC8_ALWAYS_INLINE ALLOC8_ALLOC_SIZE(2)
  static void* realloc(void* ptr, size_t sz) {
    // Check if allocator has native realloc
    if constexpr (requires(AllocatorType& a, void* p, size_t s) {
      { a.realloc(p, s) } -> std::convertible_to<void*>;
    }) {
      return getHeap()->realloc(ptr, sz);
    } else {
      // Default implementation
      if (!ptr) {
        return malloc(sz);
      }
      if (sz == 0) {
        free(ptr);
        return nullptr;
      }

      size_t oldSize = getSize(ptr);
      // If shrinking and allocator tracks sizes, we might be able to return same ptr
      // But without knowing if allocator supports in-place shrink, always reallocate

      void* newPtr = malloc(sz);
      if (newPtr) {
        size_t copySize = (oldSize < sz) ? oldSize : sz;
        std::memcpy(newPtr, ptr, copySize);
        free(ptr);
      }
      return newPtr;
    }
  }

  /**
   * Calloc with overflow check and zero-init.
   */
  ALLOC8_ALWAYS_INLINE ALLOC8_MALLOC_ATTR ALLOC8_ALLOC_SIZE(1, 2)
  static void* calloc(size_t count, size_t size) {
    // Overflow check
    size_t total = count * size;
    if (ALLOC8_UNLIKELY(size != 0 && total / size != count)) {
      return nullptr;
    }

    void* ptr = malloc(total);
    if (ALLOC8_LIKELY(ptr != nullptr)) {
      std::memset(ptr, 0, total);
    }
    return ptr;
  }
};

// ─── CONVENIENCE TYPE ALIAS ───────────────────────────────────────────────────

/**
 * Helper to create a HeapRedirect from an allocator type.
 */
template<typename AllocatorType>
using Redirect = HeapRedirect<AllocatorType>;

} // namespace alloc8

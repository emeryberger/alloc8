// alloc8/ansi_wrapper.h - ANSI C compliance wrapper
#pragma once

#include "platform.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <climits>

namespace alloc8 {

/**
 * ANSIWrapper: Template wrapper ensuring ANSI C compliance.
 *
 * Wraps an allocator to provide:
 * - Minimum alignment guarantees (16 bytes by default)
 * - Overflow detection for size calculations
 * - Proper handling of edge cases (size 0, null pointers)
 *
 * @tparam SuperHeap The underlying allocator to wrap
 * @tparam MinAlignment Minimum alignment in bytes (default: 16)
 */
template<typename SuperHeap, size_t MinAlignment = ALLOC8_MIN_ALIGNMENT>
class ANSIWrapper : public SuperHeap {
  static_assert((MinAlignment & (MinAlignment - 1)) == 0,
                "MinAlignment must be a power of 2");
  static_assert(MinAlignment >= sizeof(void*),
                "MinAlignment must be at least sizeof(void*)");

public:
  static constexpr size_t alignment = MinAlignment;

  /**
   * Allocate memory with ANSI compliance.
   * - Ensures minimum alignment
   * - Returns nullptr on overflow or failure
   * - size 0 returns minimum-sized allocation (not nullptr)
   */
  ALLOC8_ALWAYS_INLINE
  void* malloc(size_t sz) {
    // Enforce minimum size for alignment
    if (sz < alignment) {
      sz = alignment;
    }

    // Check for overflow in size rounding
    if (ALLOC8_UNLIKELY(sz > SIZE_MAX - alignment + 1)) {
      return nullptr;
    }

    // Round up to alignment
    sz = (sz + alignment - 1) & ~(alignment - 1);

    return SuperHeap::malloc(sz);
  }

  /**
   * Free memory. Handles nullptr gracefully.
   */
  ALLOC8_ALWAYS_INLINE
  void free(void* ptr) {
    if (ALLOC8_LIKELY(ptr != nullptr)) {
      SuperHeap::free(ptr);
    }
  }

  /**
   * Reallocate with ANSI semantics.
   * - ptr == nullptr: equivalent to malloc(sz)
   * - sz == 0: equivalent to free(ptr), returns nullptr
   */
  ALLOC8_ALWAYS_INLINE
  void* realloc(void* ptr, size_t sz) {
    if (!ptr) {
      return malloc(sz);
    }

    if (sz == 0) {
      free(ptr);
      return nullptr;
    }

    // Enforce minimum and alignment
    if (sz < alignment) {
      sz = alignment;
    }
    sz = (sz + alignment - 1) & ~(alignment - 1);

    // Check current size
    size_t currentSize = SuperHeap::getSize(ptr);

    // If new size fits in current allocation, return same pointer
    // (some allocators may want to shrink, but this is safe)
    if (sz <= currentSize) {
      return ptr;
    }

    // Allocate new block
    void* newPtr = SuperHeap::malloc(sz);
    if (ALLOC8_UNLIKELY(!newPtr)) {
      return nullptr;
    }

    // Copy data
    std::memcpy(newPtr, ptr, currentSize);

    // Free old block
    SuperHeap::free(ptr);

    return newPtr;
  }

  /**
   * Calloc with overflow protection.
   */
  ALLOC8_ALWAYS_INLINE
  void* calloc(size_t count, size_t size) {
    // Overflow check
    if (ALLOC8_UNLIKELY(size != 0 && count > SIZE_MAX / size)) {
      return nullptr;
    }

    size_t totalSize = count * size;
    void* ptr = malloc(totalSize);

    if (ALLOC8_LIKELY(ptr != nullptr)) {
      std::memset(ptr, 0, totalSize);
    }

    return ptr;
  }

  /**
   * Aligned allocation.
   */
  ALLOC8_ALWAYS_INLINE
  void* memalign(size_t requestedAlignment, size_t sz) {
    // Use the larger of requested and minimum alignment
    size_t actualAlignment = (requestedAlignment > alignment)
                             ? requestedAlignment : alignment;

    // Validate alignment is power of 2
    if ((actualAlignment & (actualAlignment - 1)) != 0) {
      return nullptr;
    }

    return SuperHeap::memalign(actualAlignment, sz);
  }

  /**
   * posix_memalign semantics.
   * Returns 0 on success, error code on failure.
   */
  ALLOC8_ALWAYS_INLINE
  int posix_memalign(void** memptr, size_t requestedAlignment, size_t sz) {
    *memptr = nullptr;

    // Alignment must be power of 2 and multiple of sizeof(void*)
    if (requestedAlignment < sizeof(void*) ||
        (requestedAlignment & (requestedAlignment - 1)) != 0) {
      return EINVAL;
    }

    void* ptr = memalign(requestedAlignment, sz);
    if (!ptr && sz != 0) {
      return ENOMEM;
    }

    *memptr = ptr;
    return 0;
  }

  /**
   * C11 aligned_alloc semantics.
   * Size must be multiple of alignment.
   */
  ALLOC8_ALWAYS_INLINE
  void* aligned_alloc(size_t requestedAlignment, size_t sz) {
    // C11 requires size to be multiple of alignment
    if (requestedAlignment == 0 || (sz % requestedAlignment) != 0) {
      return nullptr;
    }
    return memalign(requestedAlignment, sz);
  }

  // Forward getSize, lock, unlock to super
  using SuperHeap::getSize;
  using SuperHeap::lock;
  using SuperHeap::unlock;

private:
  // EINVAL if not defined
  static constexpr int EINVAL = 22;
  static constexpr int ENOMEM = 12;
};

} // namespace alloc8

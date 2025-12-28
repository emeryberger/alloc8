// alloc8/examples/hoard/hoard_alloc8.cpp
// Hoard allocator adapted to use alloc8 for interposition
//
// This replaces Hoard's gnuwrapper.cpp/macwrapper.cpp with alloc8's
// platform-independent interposition mechanism.

#include <cstddef>
#include <cstdio>
#include <new>

// Hoard configuration
#define HL_NO_MALLOC_SIZE_CHECKS 0

#include "heaplayers.h"

#undef __GXX_WEAK__

#if HOARD_NO_LOCK_OPT
volatile bool anyThreadCreated = true;
#else
volatile bool anyThreadCreated = false;
#endif

#include "hoardtlab.h"

// ─── HOARD HEAP INFRASTRUCTURE ───────────────────────────────────────────────
// This replicates libhoard.cpp's initialization logic exactly.

/// Maintain a single instance of the main Hoard heap.
Hoard::HoardHeapType * getMainHoardHeap() {
  static double thBuf[sizeof(Hoard::HoardHeapType) / sizeof(double) + 1];
  static auto * th = new (thBuf) Hoard::HoardHeapType;
  return th;
}

// Direct access to thread-local heap for fast path (defined in hoard_thread_hooks.cpp)
extern __thread TheCustomHeapType* theCustomHeap;
extern bool initializedTSD;

// Slow path - implemented in hoard_thread_hooks.cpp
TheCustomHeapType* getCustomHeap();

// Init buffer for early allocations (before TLS is ready)
enum { MAX_LOCAL_BUFFER_SIZE = 256 * 131072 };
static char initBuffer[MAX_LOCAL_BUFFER_SIZE];
static char * initBufferPtr = initBuffer;

// Include generic memalign implementation from Heap-Layers
#include "wrappers/generic-memalign.cpp"

// ─── ALLOC8 XXMALLOC INTERFACE ───────────────────────────────────────────────
// These functions exactly match libhoard.cpp's xxmalloc interface.

#include <alloc8/platform.h>

extern "C" {

ALLOC8_EXPORT void* xxmalloc(size_t sz) {
  // IMPORTANT: Check initializedTSD FIRST before accessing __thread variables!
  // TLS is not available during early library initialization on macOS.
  // Accessing __thread before TLS is ready causes a crash.
  if (__builtin_expect(initializedTSD, 1)) {
    // TLS is safe to access now
    if (__builtin_expect(theCustomHeap != nullptr, 1)) {
      // Fast path: direct TLS access
      void* ptr = theCustomHeap->malloc(sz);
      if (__builtin_expect(ptr != nullptr, 1)) {
        return ptr;
      }
      fprintf(stderr, "Hoard: INTERNAL FAILURE.\n");
      abort();
    }
    // TLS initialized but heap not set for this thread - use getCustomHeap()
    return getCustomHeap()->malloc(sz);
  }
  // Very early: satisfy request from init buffer before TLS is ready
  // Align to 16 bytes for ARM64 and general alignment requirements
  size_t aligned_pos = (size_t)(initBufferPtr - initBuffer);
  aligned_pos = (aligned_pos + 15) & ~(size_t)15;
  void* ptr = initBuffer + aligned_pos;
  initBufferPtr = initBuffer + aligned_pos + sz;
  if (initBufferPtr > initBuffer + MAX_LOCAL_BUFFER_SIZE) {
    abort();
  }
  return ptr;
}

ALLOC8_EXPORT void xxfree(void* ptr) {
  // Don't free init buffer allocations (check this FIRST, before TLS access)
  if (ptr >= initBuffer && ptr < initBuffer + MAX_LOCAL_BUFFER_SIZE) {
    return;
  }
  // Check initializedTSD before accessing TLS
  if (__builtin_expect(initializedTSD, 1)) {
    if (__builtin_expect(theCustomHeap != nullptr, 1)) {
      // Fast path: direct TLS access
      theCustomHeap->free(ptr);
      return;
    }
    // Slow path
    getCustomHeap()->free(ptr);
  }
  // Very early: before TLS is ready, just leak (shouldn't happen for non-init-buffer ptrs)
}

ALLOC8_EXPORT void* xxmemalign(size_t alignment, size_t sz) {
  return generic_xxmemalign(alignment, sz);
}

ALLOC8_EXPORT size_t xxmalloc_usable_size(void* ptr) {
  // Handle init buffer pointers (check FIRST, before TLS access)
  if (ptr >= initBuffer && ptr < initBuffer + MAX_LOCAL_BUFFER_SIZE) {
    // Return a conservative size estimate for init buffer allocations
    // We don't track individual sizes, so return remaining space
    return static_cast<size_t>((initBuffer + MAX_LOCAL_BUFFER_SIZE) - (char*)ptr);
  }
  // Check initializedTSD before accessing TLS
  if (__builtin_expect(initializedTSD, 1)) {
    if (__builtin_expect(theCustomHeap != nullptr, 1)) {
      return theCustomHeap->getSize(ptr);
    }
    return getCustomHeap()->getSize(ptr);
  }
  // Very early: shouldn't happen for non-init-buffer pointers
  return 0;
}

ALLOC8_EXPORT void xxmalloc_lock() {
  // Undefined for Hoard (uses fine-grained locking)
}

ALLOC8_EXPORT void xxmalloc_unlock() {
  // Undefined for Hoard
}

ALLOC8_EXPORT void* xxrealloc(void* ptr, size_t sz) {
  if (!ptr) {
    return xxmalloc(sz);
  }
  if (sz == 0) {
    xxfree(ptr);
    return nullptr;
  }
  size_t oldSize = xxmalloc_usable_size(ptr);
  void* newPtr = xxmalloc(sz);
  if (newPtr) {
    size_t copySize = (oldSize < sz) ? oldSize : sz;
    memcpy(newPtr, ptr, copySize);
    xxfree(ptr);
  }
  return newPtr;
}

ALLOC8_EXPORT void* xxcalloc(size_t count, size_t size) {
  size_t total = count * size;
  if (size != 0 && total / size != count) {
    return nullptr;
  }
  void* ptr = xxmalloc(total);
  if (ptr) {
    memset(ptr, 0, total);
  }
  return ptr;
}

} // extern "C"

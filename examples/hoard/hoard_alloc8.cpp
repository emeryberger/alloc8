// alloc8/examples/hoard/hoard_alloc8.cpp
// Hoard allocator adapted to use alloc8 for interposition
//
// This replaces Hoard's gnuwrapper.cpp/macwrapper.cpp with alloc8's
// platform-independent interposition mechanism.
// Works on Linux, macOS, and Windows.

#include <cstddef>
#include <cstdio>
#include <new>
#include <cstring>

// Platform-specific branch prediction hints
#if defined(_MSC_VER)
#define ALLOC8_LIKELY(x) (x)
#define ALLOC8_UNLIKELY(x) (x)
#else
#define ALLOC8_LIKELY(x) __builtin_expect(!!(x), 1)
#define ALLOC8_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

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

#if defined(_WIN32)
// Windows: TLS is managed by hoard_thread_hooks_win.cpp via Windows TLS API
// getCustomHeap() returns nullptr if TLS is not ready

// Forward declaration from hoard_thread_hooks_win.cpp
TheCustomHeapType* getCustomHeap();

#else
// Unix: Direct access to thread-local heap for fast path (defined in hoard_thread_hooks.cpp)
extern __thread TheCustomHeapType* theCustomHeap;
extern bool initializedTSD;

// Slow path - implemented in hoard_thread_hooks.cpp
TheCustomHeapType* getCustomHeap();
#endif

// Init buffer for early allocations (before TLS is ready)
enum { MAX_LOCAL_BUFFER_SIZE = 256 * 131072 };
static char initBuffer[MAX_LOCAL_BUFFER_SIZE];
static char * initBufferPtr = initBuffer;


// ─── ALLOC8 XXMALLOC INTERFACE ───────────────────────────────────────────────
// These functions exactly match libhoard.cpp's xxmalloc interface.

#include <alloc8/platform.h>

// Forward declarations with export (must come before generic-memalign.cpp)
extern "C" {
  ALLOC8_EXPORT void* xxmalloc(size_t sz);
  ALLOC8_EXPORT void xxfree(void* ptr);
}

// Include generic memalign implementation from Heap-Layers
#include "wrappers/generic-memalign.cpp"

extern "C" {

ALLOC8_EXPORT void* xxmalloc(size_t sz) {
#if defined(_WIN32)
  // Windows: Single TLS lookup - getCustomHeap() returns nullptr if not ready
  auto* heap = getCustomHeap();
  if (ALLOC8_LIKELY(heap != nullptr)) {
    void* ptr = heap->malloc(sz);
    if (ALLOC8_LIKELY(ptr != nullptr)) {
      return ptr;
    }
    fprintf(stderr, "Hoard: INTERNAL FAILURE.\n");
    abort();
  }
#else
  // Unix: Check initializedTSD FIRST before accessing __thread variables!
  // TLS is not available during early library initialization on macOS.
  // Accessing __thread before TLS is ready causes a crash.
  if (ALLOC8_LIKELY(initializedTSD)) {
    // TLS is safe to access now
    if (ALLOC8_LIKELY(theCustomHeap != nullptr)) {
      // Fast path: direct TLS access
      void* ptr = theCustomHeap->malloc(sz);
      if (ALLOC8_LIKELY(ptr != nullptr)) {
        return ptr;
      }
      fprintf(stderr, "Hoard: INTERNAL FAILURE.\n");
      abort();
    }
    // TLS initialized but heap not set for this thread - use getCustomHeap()
    return getCustomHeap()->malloc(sz);
  }
#endif
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
#if defined(_WIN32)
  // Windows: Single TLS lookup - getCustomHeap() returns nullptr if not ready
  auto* heap = getCustomHeap();
  if (ALLOC8_LIKELY(heap != nullptr)) {
    heap->free(ptr);
    return;
  }
#else
  // Check initializedTSD before accessing TLS
  if (ALLOC8_LIKELY(initializedTSD)) {
    if (ALLOC8_LIKELY(theCustomHeap != nullptr)) {
      // Fast path: direct TLS access
      theCustomHeap->free(ptr);
      return;
    }
    // Slow path
    getCustomHeap()->free(ptr);
  }
#endif
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
#if defined(_WIN32)
  // Windows: Single TLS lookup - getCustomHeap() returns nullptr if not ready
  auto* heap = getCustomHeap();
  if (ALLOC8_LIKELY(heap != nullptr)) {
    return heap->getSize(ptr);
  }
#else
  // Check initializedTSD before accessing TLS
  if (ALLOC8_LIKELY(initializedTSD)) {
    if (ALLOC8_LIKELY(theCustomHeap != nullptr)) {
      return theCustomHeap->getSize(ptr);
    }
    return getCustomHeap()->getSize(ptr);
  }
#endif
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

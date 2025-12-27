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

// Forward declaration - implemented in mactls.cpp or unixtls.cpp
TheCustomHeapType * getCustomHeap();
extern bool isCustomHeapInitialized();

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
  if (isCustomHeapInitialized()) {
    void* ptr = getCustomHeap()->malloc(sz);
    if (ptr == nullptr) {
      fprintf(stderr, "Hoard: INTERNAL FAILURE.\n");
      abort();
    }
    return ptr;
  }
  // Satisfy request from init buffer before TLS is ready
  void* ptr = initBufferPtr;
  initBufferPtr += sz;
  if (initBufferPtr > initBuffer + MAX_LOCAL_BUFFER_SIZE) {
    abort();
  }
  return ptr;
}

ALLOC8_EXPORT void xxfree(void* ptr) {
  // Don't free init buffer allocations
  if (ptr >= initBuffer && ptr < initBuffer + MAX_LOCAL_BUFFER_SIZE) {
    return;
  }
  getCustomHeap()->free(ptr);
}

ALLOC8_EXPORT void* xxmemalign(size_t alignment, size_t sz) {
  return generic_xxmemalign(alignment, sz);
}

ALLOC8_EXPORT size_t xxmalloc_usable_size(void* ptr) {
  // Handle init buffer pointers
  if (ptr >= initBuffer && ptr < initBuffer + MAX_LOCAL_BUFFER_SIZE) {
    return static_cast<size_t>((initBuffer + MAX_LOCAL_BUFFER_SIZE) - (char*)ptr);
  }
  return getCustomHeap()->getSize(ptr);
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

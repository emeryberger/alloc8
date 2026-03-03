// alloc8/examples/diehard/diehard_alloc8.cpp
// DieHard allocator using alloc8 for interposition
//
// Uses alloc8's header-only gnu_wrapper.h for zero-overhead interposition.

#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <cassert>
#include <cstring>

// Required by printf.h/printf.cpp
extern "C" void _putchar(char c) {
  fputc(c, stderr);
}

// The heap multiplier
enum { Numerator = 8, Denominator = 7 };

#undef __GXX_WEAK__

#include "heaplayers.h"

#include "combineheap.h"
#include "diehard.h"
#include "largeheap.h"
#include "diehardheap.h"
#include "util/bitmap.h"
#include "util/atomicbitmap.h"
#include "globalfreepool.h"
#include "objectownership.h"
#include "scalableheap.h"

// ─── PLATFORM-SPECIFIC LOCK TYPE ──────────────────────────────────────────────

#if defined(_WIN32)
typedef HL::WinLockType TheLockType;
#else
typedef PosixLockType TheLockType;
#endif

// ─── DIEHARD HEAP DEFINITION ─────────────────────────────────────────────────

class TheLargeHeap : public OneHeap<LargeHeap<MmapWrapper>> {
  typedef OneHeap<LargeHeap<MmapWrapper>> Super;
public:
  inline void* malloc(size_t sz) {
    return Super::malloc(sz);
  }
  inline auto free(void* ptr) {
    return Super::free(ptr);
  }
};

#if DIEHARD_SCALABLE

// Scalable design: Per-thread heaps with atomic bitmaps
typedef
  ANSIWrapper<
   OwnershipTrackingHeap<
    CombineHeap<DieHardHeap<Numerator, Denominator, 1048576,
                            (DIEHARD_DIEFAST == 1),
                            (DIEHARD_DIEHARDER == 1),
                            AtomicBitMap>,
                TheLargeHeap>>>
PerThreadDieHardHeap;

typedef
 ANSIWrapper<
  LockedHeap<TheLockType,
     CombineHeap<DieHardHeap<Numerator, Denominator, 1048576,
                             (DIEHARD_DIEFAST == 1),
                             (DIEHARD_DIEHARDER == 1)>,
                 TheLargeHeap>>>
FallbackDieHardHeap;

typedef
 ScalableHeap<PerThreadDieHardHeap, FallbackDieHardHeap>
TheDieHardHeap;

#else

// Non-scalable: Single global heap with lock
typedef
 ANSIWrapper<
  LockedHeap<TheLockType,
     CombineHeap<DieHardHeap<Numerator, Denominator, 1048576,
                             (DIEHARD_DIEFAST == 1),
                             (DIEHARD_DIEHARDER == 1)>,
             TheLargeHeap>>>
TheDieHardHeap;

#endif

// ─── CUSTOM HEAP TYPE WITH ALLOC8 INTERFACE ─────────────────────────────────
// Adds memalign, lock, unlock as expected by alloc8's gnu_wrapper.h

class TheCustomHeapType : public TheDieHardHeap {
public:
  // DieHard allocates power-of-two objects, naturally aligned
  inline void* memalign(size_t alignment, size_t sz) {
    return TheDieHardHeap::malloc(sz < alignment ? alignment : sz);
  }

  // Sized free: size info is available from C23 free_sized / C++14 sized delete.
  // DieHard could use the size to compute the size class index directly (O(1))
  // instead of the linear search through all size classes. For now, delegate
  // to the regular free path which handles the search internally.
  inline void free_sized(void* ptr, size_t) {
    TheDieHardHeap::free(ptr);
  }

  // Fork safety - scalable version has per-thread heaps, no global lock
  inline void lock() {
#if !DIEHARD_SCALABLE
    TheDieHardHeap::lock();
#endif
  }

  inline void unlock() {
#if !DIEHARD_SCALABLE
    TheDieHardHeap::unlock();
#endif
  }
};

// ─── HEAP SINGLETON (required by alloc8's gnu_wrapper.h) ────────────────────
// Meyers singleton pattern - compiler optimizes away redundant checks with LTO

inline static TheCustomHeapType* getCustomHeap() {
  static char buf[sizeof(TheCustomHeapType)];
  static TheCustomHeapType* heap = new (buf) TheCustomHeapType;
  return heap;
}

// ─── XXMALLOC INTERFACE (required by Heap-Layers wrappers) ──────────────────

extern "C" {

void* xxmalloc(size_t sz) {
  return getCustomHeap()->malloc(sz);
}

void xxfree(void* ptr) {
  getCustomHeap()->free(ptr);
}

void* xxmemalign(size_t alignment, size_t sz) {
  return getCustomHeap()->memalign(alignment, sz);
}

size_t xxmalloc_usable_size(void* ptr) {
  return getCustomHeap()->getSize(ptr);
}

void xxmalloc_lock() {
  getCustomHeap()->lock();
}

void xxmalloc_unlock() {
  getCustomHeap()->unlock();
}

void* xxrealloc(void* ptr, size_t sz) {
  if (ptr == nullptr) {
    return xxmalloc(sz);
  }
  if (sz == 0) {
    xxfree(ptr);
    return nullptr;
  }
  size_t oldSize = xxmalloc_usable_size(ptr);
  void* newPtr = xxmalloc(sz);
  if (newPtr != nullptr) {
    memcpy(newPtr, ptr, oldSize < sz ? oldSize : sz);
    xxfree(ptr);
  }
  return newPtr;
}

void* xxcalloc(size_t count, size_t sz) {
  size_t totalSize = count * sz;
  void* ptr = xxmalloc(totalSize);
  if (ptr != nullptr) {
    memset(ptr, 0, totalSize);
  }
  return ptr;
}

void xxfree_sized(void* ptr, size_t sz) {
  getCustomHeap()->free_sized(ptr, sz);
}

void xxfree_aligned_sized(void* ptr, size_t, size_t sz) {
  // DieHard uses power-of-two sizes, alignment doesn't affect free path
  getCustomHeap()->free_sized(ptr, sz);
}

} // extern "C"

// ─── INCLUDE PLATFORM-SPECIFIC WRAPPER ───────────────────────────────────────

#if defined(__linux__)
#include <alloc8/gnu_wrapper.h>
#elif defined(__APPLE__)
#include "macwrapper.cpp"
#elif defined(_WIN32)
// Windows uses alloc8's win_wrapper_detours.cpp linked via CMake
// The xxmalloc functions above are called by the detoured malloc functions
#endif

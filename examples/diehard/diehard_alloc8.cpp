// alloc8/examples/diehard/diehard_alloc8.cpp
// DieHard allocator adapted to use alloc8 for interposition
//
// This replaces DieHard's gnuwrapper.cpp/macwrapper.cpp with alloc8's
// platform-independent interposition mechanism.

#include <cstddef>
#include <cstdlib>
#include <new>
#include <cassert>

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
  LockedHeap<PosixLockType,
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
  LockedHeap<PosixLockType,
     CombineHeap<DieHardHeap<Numerator, Denominator, 1048576,
                             (DIEHARD_DIEFAST == 1),
                             (DIEHARD_DIEHARDER == 1)>,
             TheLargeHeap>>>
TheDieHardHeap;

#endif

class TheCustomHeapType : public TheDieHardHeap {};

inline static TheCustomHeapType* getCustomHeap(void) {
  static char buf[sizeof(TheCustomHeapType)];
  static TheCustomHeapType* _theCustomHeap =
    new (buf) TheCustomHeapType;
  return _theCustomHeap;
}

// ─── ALLOC8 ADAPTER ──────────────────────────────────────────────────────────

#include <alloc8/alloc8.h>

/// DieHardHeapAdapter: Wraps DieHard's heap for alloc8
class DieHardHeapAdapter {
public:
  void* malloc(size_t sz) {
    return getCustomHeap()->malloc(sz);
  }

  void free(void* ptr) {
    getCustomHeap()->free(ptr);
  }

  void* memalign(size_t alignment, size_t sz) {
    // DieHard allocates power-of-two objects, naturally aligned
    void* ptr = malloc(sz < alignment ? alignment : sz);
    assert(reinterpret_cast<uintptr_t>(ptr) % alignment == 0);
    return ptr;
  }

  size_t getSize(void* ptr) {
    return getCustomHeap()->getSize(ptr);
  }

  void lock() {
#if !DIEHARD_SCALABLE
    getCustomHeap()->lock();
#endif
  }

  void unlock() {
#if !DIEHARD_SCALABLE
    getCustomHeap()->unlock();
#endif
  }
};

// ─── GENERATE XXMALLOC INTERFACE ─────────────────────────────────────────────

using DieHardRedirect = alloc8::HeapRedirect<DieHardHeapAdapter>;
ALLOC8_REDIRECT(DieHardRedirect);

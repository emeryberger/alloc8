// alloc8/examples/hoard/hoard_thread_hooks.cpp
// Thread lifecycle hooks for Hoard using alloc8's pthread interposition
//
// This file implements xxthread_init() and xxthread_cleanup() which are called
// by alloc8's mac_threads.cpp (or linux_threads.cpp) when threads are created
// and destroyed.
//
// USAGE: To enable thread-aware Hoard with alloc8's thread hooks:
//   1. Include ${ALLOC8_THREAD_SOURCES} in your library
//   2. Link against this file instead of hoard_mactls.cpp
//   3. Hoard will use alloc8's pthread interposition
//
// This approach avoids the initialization timing issues that occur when
// Hoard's own pthread interposition (in mactls.cpp) conflicts with early
// library initialization.

#if !defined(__APPLE__)
// For now, this is macOS only. Linux would use a similar approach.
#error "This file is currently for macOS only"
#endif

#include <pthread.h>

#include "heaplayers.h"
#include "hoard/hoardtlab.h"

// From hoard_alloc8.cpp
extern Hoard::HoardHeapType* getMainHoardHeap();

// ─── THREAD-LOCAL STORAGE ────────────────────────────────────────────────────
// Use pthread_key for TLS since __thread can have issues during early init

static pthread_key_t theHeapKey;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static bool initializedTSD = false;

static void deleteThatHeap(void* p) {
  if (p) {
    reinterpret_cast<TheCustomHeapType*>(p)->clear();
    getMainHoardHeap()->free(p);
    getMainHoardHeap()->releaseHeap();
  }
}

static void make_heap_key() {
  pthread_key_create(&theHeapKey, deleteThatHeap);
}

static bool initTSD() {
  if (!initializedTSD) {
    pthread_once(&key_once, make_heap_key);
    initializedTSD = true;
  }
  return true;
}

// ─── EXPORTED FUNCTIONS FOR HOARD ────────────────────────────────────────────

bool isCustomHeapInitialized() {
  return initializedTSD;
}

static TheCustomHeapType* initializeCustomHeap() {
  TheCustomHeapType* heap =
      reinterpret_cast<TheCustomHeapType*>(pthread_getspecific(theHeapKey));
  if (heap == nullptr) {
    size_t sz = sizeof(TheCustomHeapType);
    char* mh = reinterpret_cast<char*>(getMainHoardHeap()->malloc(sz));
    heap = new (mh) TheCustomHeapType(getMainHoardHeap());
    pthread_setspecific(theHeapKey, heap);
  }
  return heap;
}

TheCustomHeapType* getCustomHeap() {
  initTSD();
  TheCustomHeapType* heap =
      reinterpret_cast<TheCustomHeapType*>(pthread_getspecific(theHeapKey));
  if (heap == nullptr) {
    heap = initializeCustomHeap();
  }
  return heap;
}

// ─── ALLOC8 THREAD LIFECYCLE HOOKS ───────────────────────────────────────────
// These are called by alloc8's mac_threads.cpp when pthread_create/exit happen

extern volatile bool anyThreadCreated;

extern "C" {

// Called by alloc8 when a new thread starts (before user's thread function)
void xxthread_init(void) {
  // Initialize this thread's TLAB
  getCustomHeap();
  // Try to assign this thread to an unused heap
  getMainHoardHeap()->findUnusedHeap();
}

// Called by alloc8 when a thread is about to exit
void xxthread_cleanup(void) {
  TheCustomHeapType* heap = getCustomHeap();
  if (heap) {
    // Flush the TLAB
    heap->clear();
    // Release the assigned heap back to the pool
    getMainHoardHeap()->releaseHeap();
  }
}

// Provide the thread-created flag for alloc8's lock optimization
volatile int xxthread_created_flag = 0;

} // extern "C"

// Sync alloc8's flag with Hoard's anyThreadCreated
// This is a bit of a hack - ideally Hoard would use xxthread_created_flag directly
__attribute__((constructor(300)))
static void sync_thread_flag() {
  // If alloc8's flag is set, update Hoard's flag
  if (xxthread_created_flag) {
    anyThreadCreated = true;
  }
}

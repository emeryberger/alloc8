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

#if !defined(__APPLE__) && !defined(__linux__)
#error "This file is for macOS and Linux only"
#endif

#include <pthread.h>

#include "heaplayers.h"
#include "hoard/hoardtlab.h"

// From hoard_alloc8.cpp
extern Hoard::HoardHeapType* getMainHoardHeap();

// ─── THREAD-LOCAL STORAGE ────────────────────────────────────────────────────
// Use __thread for fast TLS access (much faster than pthread_getspecific)
// These are defined here but also extern'd in hoard_alloc8.cpp for direct access

__thread TheCustomHeapType* theCustomHeap = nullptr;
bool initializedTSD = false;

// ─── EXPORTED FUNCTIONS FOR HOARD ────────────────────────────────────────────

bool isCustomHeapInitialized() {
  return initializedTSD;
}

static TheCustomHeapType* initializeCustomHeap() {
  if (theCustomHeap == nullptr) {
    size_t sz = sizeof(TheCustomHeapType);
    char* mh = reinterpret_cast<char*>(getMainHoardHeap()->malloc(sz));
    theCustomHeap = new (mh) TheCustomHeapType(getMainHoardHeap());
  }
  return theCustomHeap;
}

TheCustomHeapType* getCustomHeap() {
  if (__builtin_expect(theCustomHeap != nullptr, 1)) {
    return theCustomHeap;
  }
  initializedTSD = true;
  return initializeCustomHeap();
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
  if (theCustomHeap) {
    // Flush the TLAB
    theCustomHeap->clear();
    // Release the assigned heap back to the pool
    getMainHoardHeap()->releaseHeap();
    // Free the heap structure
    getMainHoardHeap()->free(theCustomHeap);
    theCustomHeap = nullptr;
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

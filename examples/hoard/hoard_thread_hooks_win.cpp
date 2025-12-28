// alloc8/examples/hoard/hoard_thread_hooks_win.cpp
// Windows thread lifecycle hooks for Hoard
//
// This file implements the Windows-specific thread management for Hoard.
// Based on Hoard's wintls.cpp pattern, using DLL_THREAD_ATTACH/DETACH
// notifications for thread lifecycle events.
//
// This is a self-contained implementation that provides its own DllMain.

#ifndef _WIN32
#error "This file is for Windows only"
#endif

#include <windows.h>
#include <new>

#include "heaplayers.h"
#include "hoard/hoardtlab.h"

// From hoard_alloc8.cpp
extern Hoard::HoardHeapType* getMainHoardHeap();

// ─── THREAD-LOCAL STORAGE ────────────────────────────────────────────────────
// Use Windows TLS API for thread-local heap pointer

static DWORD g_tlsIndex = TLS_OUT_OF_INDEXES;
static bool g_tlsInitialized = false;

// ─── EXPORTED FUNCTIONS FOR HOARD ────────────────────────────────────────────

bool isCustomHeapInitialized() {
  if (!g_tlsInitialized || g_tlsIndex == TLS_OUT_OF_INDEXES) {
    return false;
  }
  return TlsGetValue(g_tlsIndex) != nullptr;
}

static TheCustomHeapType* initializeCustomHeap() {
  auto* mainHeap = getMainHoardHeap();
  size_t sz = sizeof(TheCustomHeapType);
  char* mh = reinterpret_cast<char*>(mainHeap->malloc(sz));
  auto* heap = new (mh) TheCustomHeapType(mainHeap);
  TlsSetValue(g_tlsIndex, heap);
  return heap;
}

TheCustomHeapType* getCustomHeap() {
  if (!g_tlsInitialized || g_tlsIndex == TLS_OUT_OF_INDEXES) {
    // TLS not ready yet - shouldn't happen normally
    return nullptr;
  }

  auto* heap = static_cast<TheCustomHeapType*>(TlsGetValue(g_tlsIndex));
  if (heap != nullptr) {
    return heap;
  }

  return initializeCustomHeap();
}

// ─── ALLOC8 THREAD LIFECYCLE HOOKS ───────────────────────────────────────────
// These are called by alloc8's win_threads.cpp via DllMain

extern volatile bool anyThreadCreated;

extern "C" {

// Called when a new thread starts (via DLL_THREAD_ATTACH)
void xxthread_init(void) {
  // Initialize this thread's TLAB
  getCustomHeap();

  // Try to assign this thread to an unused heap
  auto np = HL::CPUInfo::computeNumProcessors();
  if (np == 1) {
    getMainHoardHeap()->chooseZero();
  } else {
    getMainHoardHeap()->findUnusedHeap();
  }
}

// Called when a thread is about to exit (via DLL_THREAD_DETACH)
void xxthread_cleanup(void) {
  if (!g_tlsInitialized || g_tlsIndex == TLS_OUT_OF_INDEXES) {
    return;
  }

  auto* heap = static_cast<TheCustomHeapType*>(TlsGetValue(g_tlsIndex));
  if (heap) {
    // Flush the TLAB
    heap->clear();

    // Release the assigned heap back to the pool
    auto np = HL::CPUInfo::computeNumProcessors();
    if (np != 1) {
      getMainHoardHeap()->releaseHeap();
    }

    // Free the heap structure
    getMainHoardHeap()->free(heap);
    TlsSetValue(g_tlsIndex, nullptr);
  }
}

// Provide the thread-created flag for alloc8's lock optimization
volatile int xxthread_created_flag = 0;

} // extern "C"

// ─── DLL ENTRY POINT ─────────────────────────────────────────────────────────

extern "C" void InitializeAlloc8();

BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  static auto np = HL::CPUInfo::computeNumProcessors();

  switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
      {
        // Pin this DLL in memory to prevent unloading
        HMODULE hSelf = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)DllMain, &hSelf);

        // Allocate TLS index
        g_tlsIndex = TlsAlloc();
        if (g_tlsIndex == TLS_OUT_OF_INDEXES) {
          return FALSE;
        }
        g_tlsInitialized = true;

        // Initialize alloc8 (sets up detours)
        InitializeAlloc8();

        // Force creation of the main thread's heap
        volatile auto* ch = getCustomHeap();
        (void)ch;
      }
      break;

    case DLL_THREAD_ATTACH:
      {
        anyThreadCreated = true;
        xxthread_init();
      }
      break;

    case DLL_THREAD_DETACH:
      {
        xxthread_cleanup();
      }
      break;

    case DLL_PROCESS_DETACH:
      {
        if (lpvReserved == nullptr) {
          // Dynamic unload (FreeLibrary)
          if (g_tlsIndex != TLS_OUT_OF_INDEXES) {
            TlsFree(g_tlsIndex);
            g_tlsIndex = TLS_OUT_OF_INDEXES;
          }
        } else {
          // Process exit - force immediate termination to avoid crashes
          // from detoured functions pointing to invalid memory
          TerminateProcess(GetCurrentProcess(), 0);
        }
      }
      break;
  }

  return TRUE;
}

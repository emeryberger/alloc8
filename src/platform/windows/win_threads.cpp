// alloc8/src/platform/windows/win_threads.cpp
// Windows thread lifecycle hooks for thread-aware allocators
//
// This file provides DllMain integration functions that allocators can call
// to implement thread lifecycle hooks on Windows.
//
// Unlike Linux/macOS which interpose pthread_create/pthread_exit, Windows uses
// DLL_THREAD_ATTACH/DLL_THREAD_DETACH notifications in DllMain. This is simpler
// and more reliable than hooking CreateThread.
//
// Usage: The allocator's DLL should call Alloc8OnThreadAttach() from
// DLL_THREAD_ATTACH and Alloc8OnThreadDetach() from DLL_THREAD_DETACH.

#ifndef _WIN32
#error "This file is for Windows only"
#endif

#include <windows.h>
#include <atomic>

// ─── WEAK SYMBOL SIMULATION ─────────────────────────────────────────────────
// MSVC doesn't have __attribute__((weak)), so we use /ALTERNATENAME to provide
// default implementations that can be overridden by the allocator.

extern "C" {
  // Allocator-provided hooks - declare but don't define (allocator provides these)
  void xxthread_init(void);
  void xxthread_cleanup(void);

  // Allocator-provided thread-created flag
  extern volatile int xxthread_created_flag;
}

// Default implementations (used if allocator doesn't provide them)
// These are linked via /ALTERNATENAME pragmas below

static void xxthread_init_default(void) {
  // No-op default
}

static void xxthread_cleanup_default(void) {
  // No-op default
}

static volatile int xxthread_created_flag_default = 0;

// MSVC weak symbol simulation using /ALTERNATENAME
// If the allocator provides xxthread_init, it will be used; otherwise xxthread_init_default
#pragma comment(linker, "/ALTERNATENAME:xxthread_init=xxthread_init_default")
#pragma comment(linker, "/ALTERNATENAME:xxthread_cleanup=xxthread_cleanup_default")
#pragma comment(linker, "/ALTERNATENAME:xxthread_created_flag=xxthread_created_flag_default")

// ─── INITIALIZATION GUARD ────────────────────────────────────────────────────
// Ensure thread hooks don't activate until malloc is fully ready.
// This prevents crashes during early library initialization.

static std::atomic<bool> alloc8_thread_ready{false};

// Internal flag if allocator doesn't provide one
static volatile int alloc8_internal_thread_flag = 0;

static volatile int* get_thread_created_flag() {
  // Check if allocator provided the flag by comparing addresses
  // If using default, both will point to our default
  return &xxthread_created_flag;
}

// ─── PUBLIC API ─────────────────────────────────────────────────────────────

extern "C" {

// Initialize thread hooks - call after malloc is ready
__declspec(dllexport) void Alloc8ThreadHooksInit(void) {
  alloc8_thread_ready.store(true, std::memory_order_release);
}

// Check if thread hooks are ready
__declspec(dllexport) bool Alloc8ThreadHooksReady(void) {
  return alloc8_thread_ready.load(std::memory_order_acquire);
}

// Call from DLL_THREAD_ATTACH in allocator's DllMain
__declspec(dllexport) void Alloc8OnThreadAttach(void) {
  if (!alloc8_thread_ready.load(std::memory_order_acquire)) {
    return;
  }

  // Mark that threads are being created (for lock optimization)
  volatile int* flag = get_thread_created_flag();
  *flag = 1;

  // Call allocator's thread init hook
  xxthread_init();
}

// Call from DLL_THREAD_DETACH in allocator's DllMain
__declspec(dllexport) void Alloc8OnThreadDetach(void) {
  if (!alloc8_thread_ready.load(std::memory_order_acquire)) {
    return;
  }

  // Call allocator's cleanup hook
  xxthread_cleanup();
}

} // extern "C"

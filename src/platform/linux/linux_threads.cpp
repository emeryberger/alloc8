// alloc8/src/platform/linux/linux_threads.cpp
// Linux pthread interposition for thread-aware allocators
//
// This file provides pthread_create/pthread_exit interposition that calls
// the allocator's xxthread_init/xxthread_cleanup hooks.
//
// Based on the macOS implementation (mac_threads.cpp) but uses GNU strong
// symbol aliasing and dlsym for Linux.

#ifndef __linux__
#error "This file is for Linux only"
#endif

#include <pthread.h>
#include <dlfcn.h>
#include <atomic>
#include <cstdlib>

// ─── WEAK SYMBOL DETECTION ───────────────────────────────────────────────────
// These are defined by the allocator if it wants thread awareness.
// If not defined, they resolve to nullptr and we skip interposition logic.

extern "C" {
  // Allocator-provided hooks (weak - may not exist)
  __attribute__((weak)) void xxthread_init(void);
  __attribute__((weak)) void xxthread_cleanup(void);

  // Allocator-provided thread-created flag (weak - may not exist)
  __attribute__((weak)) extern volatile int xxthread_created_flag;

  // Our own internal allocator functions
  void* xxmalloc(size_t);
  void xxfree(void*);
}

// Internal flag if allocator doesn't provide one
static volatile int alloc8_internal_thread_flag = 0;

static volatile int* get_thread_created_flag() {
  if (&xxthread_created_flag != nullptr) {
    return &xxthread_created_flag;
  }
  return &alloc8_internal_thread_flag;
}

// ─── REAL PTHREAD FUNCTIONS ─────────────────────────────────────────────────
// Get pointers to the real pthread functions via dlsym

using pthread_create_fn = int (*)(pthread_t*, const pthread_attr_t*,
                                   void* (*)(void*), void*);
using pthread_exit_fn = void (*)(void*);

static pthread_create_fn real_pthread_create = nullptr;
static pthread_exit_fn real_pthread_exit = nullptr;
static std::atomic<bool> real_funcs_initialized{false};

// Thread-local guard to prevent recursion during initialization
static __thread int in_init = 0;

static void init_real_funcs() {
  if (real_funcs_initialized.load(std::memory_order_acquire)) {
    return;
  }
  if (in_init) {
    return;
  }
  in_init = 1;

  real_pthread_create = (pthread_create_fn)dlsym(RTLD_NEXT, "pthread_create");
  real_pthread_exit = (pthread_exit_fn)dlsym(RTLD_NEXT, "pthread_exit");

  real_funcs_initialized.store(true, std::memory_order_release);
  in_init = 0;
}

// ─── INITIALIZATION GUARD ────────────────────────────────────────────────────
// Ensure pthread hooks don't activate until malloc is fully ready.
// This prevents crashes during early library initialization.

static std::atomic<bool> alloc8_pthread_ready{false};

__attribute__((constructor(200)))  // Run after malloc init (priority 101)
static void alloc8_pthread_hooks_init() {
  init_real_funcs();
  alloc8_pthread_ready.store(true, std::memory_order_release);
}

static bool pthread_hooks_ready() {
  return alloc8_pthread_ready.load(std::memory_order_acquire);
}

static bool has_thread_hooks() {
  return &xxthread_init != nullptr || &xxthread_cleanup != nullptr;
}

// ─── THREAD WRAPPER ──────────────────────────────────────────────────────────

namespace {

struct ThreadWrapper {
  void* (*user_func)(void*);
  void* user_arg;
};

// Trampoline function that wraps the user's thread function
void* alloc8_thread_trampoline(void* arg) {
  ThreadWrapper* wrapper = static_cast<ThreadWrapper*>(arg);

  // Extract user function and argument
  auto user_func = wrapper->user_func;
  auto user_arg = wrapper->user_arg;

  // Free the wrapper (allocated in alloc8_pthread_create)
  xxfree(wrapper);

  // Call allocator's thread init hook
  if (&xxthread_init != nullptr) {
    xxthread_init();
  }

  // Run the user's thread function
  void* result = user_func(user_arg);

  // Call allocator's cleanup hook
  if (&xxthread_cleanup != nullptr) {
    xxthread_cleanup();
  }

  return result;
}

} // anonymous namespace

// ─── PTHREAD INTERPOSITION ───────────────────────────────────────────────────

extern "C" {

// Custom pthread_create that wraps thread function
static int alloc8_pthread_create(
    pthread_t* thread,
    const pthread_attr_t* attr,
    void* (*start_routine)(void*),
    void* arg)
{
  // Ensure real functions are initialized
  init_real_funcs();

  // If not ready or no hooks, pass through to real pthread_create
  if (!pthread_hooks_ready() || !has_thread_hooks() || !real_pthread_create) {
    if (real_pthread_create) {
      return real_pthread_create(thread, attr, start_routine, arg);
    }
    // Fallback if dlsym failed (shouldn't happen)
    return -1;
  }

  // Mark that threads are being created (for lock optimization)
  volatile int* flag = get_thread_created_flag();
  *flag = 1;

  // Allocate wrapper for thread function and argument
  ThreadWrapper* wrapper = static_cast<ThreadWrapper*>(
      xxmalloc(sizeof(ThreadWrapper)));
  if (!wrapper) {
    // Fall back to direct call if allocation fails
    return real_pthread_create(thread, attr, start_routine, arg);
  }

  wrapper->user_func = start_routine;
  wrapper->user_arg = arg;

  // Create thread with our trampoline
  int result = real_pthread_create(thread, attr, alloc8_thread_trampoline, wrapper);

  if (result != 0) {
    // Creation failed, free wrapper
    xxfree(wrapper);
  }

  return result;
}

// Custom pthread_exit that calls cleanup hook
static void alloc8_pthread_exit(void* value_ptr) __attribute__((__noreturn__));
static void alloc8_pthread_exit(void* value_ptr) {
  // Call cleanup hook if ready and provided
  if (pthread_hooks_ready() && &xxthread_cleanup != nullptr) {
    xxthread_cleanup();
  }

  // Ensure real function is available
  init_real_funcs();

  // Call real pthread_exit (never returns)
  if (real_pthread_exit) {
    real_pthread_exit(value_ptr);
  }

  // Should never reach here, but satisfy noreturn
  __builtin_unreachable();
}

// ─── STRONG SYMBOL ALIASING ─────────────────────────────────────────────────
// Override the standard pthread functions with our versions

#define ATTRIBUTE_EXPORT __attribute__((visibility("default")))

// Strong aliases for pthread functions
ATTRIBUTE_EXPORT int pthread_create(
    pthread_t* thread,
    const pthread_attr_t* attr,
    void* (*start_routine)(void*),
    void* arg) {
  return alloc8_pthread_create(thread, attr, start_routine, arg);
}

ATTRIBUTE_EXPORT void pthread_exit(void* value_ptr) {
  alloc8_pthread_exit(value_ptr);
}

} // extern "C"

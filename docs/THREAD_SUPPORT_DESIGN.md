# alloc8 Thread Lifecycle Support Design

## Overview

This document outlines a design for adding optional thread lifecycle hooks to alloc8,
allowing allocators to be notified when threads are created and destroyed.

## Motivation

Thread-aware allocators like Hoard need to:
1. Initialize per-thread heaps (TLABs) when threads start
2. Clean up thread-local state when threads exit
3. Track thread creation for lock optimizations

Currently, Hoard implements this via pthread interposition in mactls.cpp/unixtls.cpp.
This causes initialization timing issues when combined with alloc8's malloc interposition.

By factoring pthread interposition into alloc8:
- Allocators get a clean interface for thread awareness
- alloc8 handles platform-specific mechanics and initialization ordering
- The pthread interposition happens after malloc is fully ready
- Code reuse across allocators

## Proposed Interface

### Allocator-Side Callbacks

Allocators that want thread awareness implement these optional functions:

```cpp
extern "C" {
  // Called in the context of a newly created thread, before the thread
  // function runs. Use this to initialize per-thread heap structures.
  // Optional - if not provided, no thread-create notification occurs.
  void xxthread_init(void);

  // Called when a thread is about to exit. Use this to flush TLABs
  // and release per-thread resources.
  // Optional - if not provided, no thread-exit notification occurs.
  void xxthread_cleanup(void);
}
```

### Detecting Thread Awareness

alloc8 uses weak symbols to detect if the allocator provides thread hooks:

```cpp
// In alloc8's thread wrapper
extern "C" __attribute__((weak)) void xxthread_init(void);
extern "C" __attribute__((weak)) void xxthread_cleanup(void);

static bool has_thread_hooks() {
  return xxthread_init != nullptr || xxthread_cleanup != nullptr;
}
```

### Platform Implementation

#### macOS

alloc8 provides `alloc8_pthread_create` and `alloc8_pthread_exit` that:
1. Call the allocator's hooks if provided
2. Delegate to the real pthread functions

```cpp
// In mac_wrapper.cpp (or new mac_threads.cpp)

extern "C" int alloc8_pthread_create(
    pthread_t* thread,
    const pthread_attr_t* attr,
    void* (*start_routine)(void*),
    void* arg)
{
  // Wrapper struct allocated from our heap
  struct thread_wrapper {
    void* (*user_func)(void*);
    void* user_arg;
  };

  auto* wrapper = (thread_wrapper*)xxmalloc(sizeof(thread_wrapper));
  wrapper->user_func = start_routine;
  wrapper->user_arg = arg;

  return pthread_create(thread, attr, alloc8_thread_trampoline, wrapper);
}

static void* alloc8_thread_trampoline(void* arg) {
  auto* wrapper = (thread_wrapper*)arg;
  auto user_func = wrapper->user_func;
  auto user_arg = wrapper->user_arg;

  // Initialize thread-local state
  if (xxthread_init) {
    xxthread_init();
  }

  // Run user's thread function
  void* result = user_func(user_arg);

  // Cleanup before returning
  if (xxthread_cleanup) {
    xxthread_cleanup();
  }

  xxfree(wrapper);
  return result;
}

extern "C" void alloc8_pthread_exit(void* value_ptr) {
  if (xxthread_cleanup) {
    xxthread_cleanup();
  }
  pthread_exit(value_ptr);
}

// Interposition - only if thread hooks are provided
// This is the tricky part - we need conditional interposition
```

#### Linux

Similar approach using dlsym(RTLD_NEXT) to find the real pthread functions.

#### Windows

Use DllMain with DLL_THREAD_ATTACH/DETACH notifications.

## Initialization Ordering

The key insight from debugging the Hoard crash: pthread interposition must happen
AFTER malloc interposition is fully ready. The order should be:

1. Library loads, `__interpose` section is processed
2. malloc/free are now interposed to our functions
3. Constructor with priority 101 runs (if needed for early init)
4. pthread interposition becomes active (via constructor or late binding)

### Safe Initialization Pattern

```cpp
// Flag to track if we're safe to do thread operations
static std::atomic<bool> alloc8_pthread_ready{false};

__attribute__((constructor(200)))  // Run after malloc init (priority 101)
static void alloc8_init_pthread_hooks() {
  alloc8_pthread_ready.store(true, std::memory_order_release);
}

extern "C" int alloc8_pthread_create(...) {
  // If not ready, pass through to real pthread_create
  if (!alloc8_pthread_ready.load(std::memory_order_acquire)) {
    return pthread_create(thread, attr, start_routine, arg);
  }
  // Otherwise use our wrapper
  ...
}
```

## Conditional Interposition

A challenge: we want pthread interposition only when the allocator provides hooks.
Options:

### Option A: Always Interpose, Check at Runtime

```cpp
extern "C" int alloc8_pthread_create(...) {
  if (!has_thread_hooks()) {
    return pthread_create(thread, attr, start_routine, arg);
  }
  // Do wrapper logic
}
```

Pro: Simple
Con: Slight overhead even when not needed

### Option B: Separate Library

Create `alloc8_threads` as optional linkage:

```cmake
# Allocator links this only if it wants thread awareness
target_link_libraries(myalloc PRIVATE alloc8::threads)
```

Pro: Zero overhead when not used
Con: More complex build

### Option C: Build-Time Configuration

```cmake
option(ALLOC8_THREAD_HOOKS "Enable pthread interposition" OFF)
```

Pro: Explicit opt-in
Con: Less flexible

**Recommendation**: Option A for simplicity. The runtime check is negligible
compared to thread creation overhead.

## Example: Hoard Integration

With this design, Hoard's integration becomes:

```cpp
// hoard_alloc8.cpp

// Existing xxmalloc/xxfree interface...

// Thread lifecycle hooks
extern "C" void xxthread_init() {
  // Initialize TLAB for this thread
  getCustomHeap();
  getMainHoardHeap()->findUnusedHeap();
}

extern "C" void xxthread_cleanup() {
  TheCustomHeapType* heap = getCustomHeap();
  heap->clear();
  getMainHoardHeap()->releaseHeap();
}
```

No need for custom mactls.cpp wrapper - alloc8 handles the interposition.

## Files to Create/Modify

1. **include/alloc8/thread_hooks.h** - Documentation of xxthread_init/xxthread_cleanup
2. **src/platform/macos/mac_threads.cpp** - macOS pthread interposition
3. **src/platform/linux/linux_threads.cpp** - Linux pthread interposition
4. **src/platform/windows/win_threads.cpp** - Windows DllMain hooks
5. **CMakeLists.txt** - Add ALLOC8_THREAD_SOURCES variable

## Open Questions

1. Should we also expose `anyThreadCreated` flag for lock optimization?
2. How to handle thread-local storage (TLS) - should alloc8 provide helpers?
3. What about thread pools and thread reuse scenarios?
4. Should there be a hook for thread-to-heap assignment?

## Implementation Priority

1. macOS implementation (most pressing due to current Hoard issues)
2. Linux implementation
3. Windows implementation
4. Documentation and examples

# CLAUDE.md - AI Assistant Guide for alloc8

## Project Overview

alloc8 is a platform-independent allocator interposition library. It provides the infrastructure to replace system malloc/free with custom allocators via LD_PRELOAD (Linux), DYLD_INSERT_LIBRARIES (macOS), or Detours (Windows).

Factored from patterns in Hoard, DieHard, and Scalene (all using Heap-Layers).

## Build Commands

```bash
# Configure with tests and examples
cmake -B build -DALLOC8_BUILD_TESTS=ON -DALLOC8_BUILD_EXAMPLES=ON

# Build
cmake --build build

# Run tests
ctest --test-dir build

# Test interposition (macOS)
DYLD_INSERT_LIBRARIES=build/examples/simple_heap/libsimple_heap.dylib ./build/tests/test_basic_alloc

# Test interposition (Linux)
LD_PRELOAD=build/examples/simple_heap/libsimple_heap.so ./build/tests/test_basic_alloc
```

## Building DieHard/Hoard Examples

Dependencies are automatically fetched via CMake FetchContent.

```bash
# DieHard example - use GCC 11+ for best LTO performance
CXX=/opt/gcc-11/bin/g++ CC=/opt/gcc-11/bin/gcc cmake -B build \
  -DALLOC8_BUILD_EXAMPLES=ON -DALLOC8_BUILD_DIEHARD_EXAMPLE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Test DieHard (Linux)
LD_PRELOAD=build/examples/diehard/libdiehard_alloc8.so ./test_program

# Test DieHard (macOS)
DYLD_INSERT_LIBRARIES=build/examples/diehard/libdiehard_alloc8.dylib ./test_program

# Hoard example (working on Linux, has macOS init timing issues)
cmake -B build -DALLOC8_BUILD_EXAMPLES=ON -DALLOC8_BUILD_HOARD_EXAMPLE=ON
cmake --build build

# Test Hoard (Linux)
LD_PRELOAD=build/examples/hoard/libhoard_alloc8.so ./test_program
```

### DieHard Zero-Overhead Build

The DieHard example achieves **near-zero overhead** (~10% single-threaded, within variance multi-threaded) compared to the original DieHard by:

1. **Using alloc8's gnu_wrapper.h** - Header-only wrapper that calls `getCustomHeap()` directly, enabling full inlining with LTO
2. **CMake IPO support** - Uses `INTERPROCEDURAL_OPTIMIZATION` for cross-unit inlining
3. **Version script** - Proper symbol visibility for optimal linker optimization
4. **C++23 standard** - Enables latest optimizations

Performance comparison (threadtest, 8 threads, 1000 iterations, 8000 objects):
- alloc8 DieHard: ~0.32s
- Original DieHard: ~0.32s (within variance)

## Architecture

### Key Components

1. **`include/alloc8/alloc8.h`** - Main header with `ALLOC8_REDIRECT`, `ALLOC8_THREAD_REDIRECT`, and `ALLOC8_REDIRECT_WITH_THREADS` macros
2. **`include/alloc8/allocator_traits.h`** - `HeapRedirect<T>` and `ThreadRedirect<T>` templates, `Allocator` and `ThreadAwareAllocator` concepts
3. **`include/alloc8/platform.h`** - Platform detection and compiler attribute macros
4. **`include/alloc8/thread_hooks.h`** - Thread lifecycle hooks interface documentation
5. **`include/alloc8/gnu_wrapper.h`** - Header-only Linux wrapper for zero-overhead interposition (advanced)

### Platform Wrappers

| Platform | File | Mechanism |
|----------|------|-----------|
| Linux | `src/platform/linux/gnu_wrapper.cpp` | Strong symbol aliasing |
| Linux | `include/alloc8/gnu_wrapper.h` | Header-only (zero-overhead) |
| macOS | `src/platform/macos/mac_wrapper.cpp` | `__DATA,__interpose` section |
| Windows | `src/platform/windows/win_wrapper_detours.cpp` | Microsoft Detours |

### Thread Interposition

| Platform | File | Mechanism |
|----------|------|-----------|
| Linux | `src/platform/linux/linux_threads.cpp` | Strong symbol aliasing for pthread_create/pthread_exit |
| macOS | `src/platform/macos/mac_threads.cpp` | Interpose pthread functions |

### xxmalloc Interface

The bridge between platform wrappers and user allocators:
- `xxmalloc(size_t)` - Allocate memory
- `xxfree(void*)` - Free memory
- `xxmemalign(size_t, size_t)` - Aligned allocation
- `xxmalloc_usable_size(void*)` - Get allocation size
- `xxmalloc_lock()` / `xxmalloc_unlock()` - Fork safety
- `xxrealloc(void*, size_t)` - Reallocation
- `xxcalloc(size_t, size_t)` - Zeroed allocation

### xxthread Interface (Optional)

For thread-aware allocators:
- `xxthread_init()` - Called when a new thread starts
- `xxthread_cleanup()` - Called when a thread exits
- `xxthread_created_flag` - Set when first thread is created (for lock optimization)

## Important Implementation Details

### macOS Specifics
- `mac_zones.cpp` is `#included` by `mac_wrapper.cpp`, NOT compiled separately
- macOS does NOT have `memalign()` - only `posix_memalign()` - do not interpose memalign
- ARM64 macOS uses 16KB pages (`ALLOC8_PAGE_SIZE`)
- Full `malloc_zone_t` implementation enables interposition WITHOUT `DYLD_FORCE_FLAT_NAMESPACE`
- Must provide forward declarations for interposed functions (vfree, _malloc_fork_*, C++ operators)
- C++ operator mangled names: `_Znwm` (new), `_Znam` (new[]), `_ZdlPv` (delete), `_ZdaPv` (delete[])

### Linux Specifics
- Uses version script (`version_script.map`) for GLIBC symbol versioning
- Requires `-Bsymbolic` linker flag to avoid infinite recursion
- Compiler flags: `-fno-builtin-malloc`, `-fno-builtin-free`, etc.
- Thread hooks (`linux_threads.cpp`) use strong symbol aliasing for pthread_create/pthread_exit
- Requires clang or GCC 10+ for C++20 support (default GCC 7.x on Amazon Linux 2 doesn't work)
- For zero-overhead: Use `gnu_wrapper.h` with GCC 11+ and full LTO

### Windows Specifics
- Microsoft Detours fetched via CMake FetchContent
- Must handle "foreign" pointers allocated before hooks installed
- Uses SEH for safe foreign pointer detection

## Common Issues and Solutions

### 1. Infinite Recursion
**Problem:** Allocator calling malloc internally causes infinite loop under LD_PRELOAD.
**Solution:** Use raw `mmap`/`munmap` (POSIX) or `VirtualAlloc`/`VirtualFree` (Windows) as backing store.

### 2. Missing ALLOC8_COMMON_SOURCES
**Problem:** Link errors for calloc/realloc/strdup wrappers.
**Solution:** CMake users must include `${ALLOC8_COMMON_SOURCES}` in their target sources.

### 3. C++20 [[likely]]/[[unlikely]] in Macros
**Problem:** Attribute placement errors in macro contexts.
**Solution:** Use `__builtin_expect(!!(x), 1)` instead of `[[likely]]` attributes.

### 4. macOS malloc_size Returns 0
**Problem:** ObjC runtime crashes with "corrupt data pointer" when `malloc_size` returns 0.
**Solution:** Always return a valid size for allocated pointers, including init buffer allocations.

### 5. Hoard Init Buffer Timing (macOS only)
**Problem:** Hoard uses an init buffer for allocations before TLS is ready. Complex interaction with alloc8.
**Solution:** The Hoard example works correctly on Linux but has unresolved macOS timing issues. DieHard (simpler singleton pattern) works on both platforms.

### 6. Works in Debugger but Crashes Otherwise (macOS only)
**Problem:** SIGBUS (exit code 138) outside debugger, works under lldb.
**Cause:** Timing-dependent initialization race condition.
**Status:** Known issue with Hoard example on macOS. Works fine on Linux.

## Integration Patterns

### Pattern 1: HeapRedirect (Recommended for Simple Heaps)

Use the `HeapRedirect` template and `ALLOC8_REDIRECT` macro for simple allocators.

```cpp
#include <alloc8/alloc8.h>

class MyHeap {
public:
  void* malloc(size_t sz);
  void free(void* ptr);
  void* memalign(size_t align, size_t sz);
  size_t getSize(void* ptr);
  void lock();    // For fork safety
  void unlock();  // For fork safety
};

using MyRedirect = alloc8::HeapRedirect<MyHeap>;
ALLOC8_REDIRECT(MyRedirect);
```

CMake:
```cmake
add_library(myalloc SHARED
  my_allocator.cpp
  ${ALLOC8_INTERPOSE_SOURCES}
)
target_link_libraries(myalloc PRIVATE alloc8::interpose)
```

### Pattern 2: HeapRedirect with Thread Hooks (For Per-Thread State)

For allocators that need per-thread state (TLABs, thread-local caches), add thread hooks.

```cpp
#include <alloc8/alloc8.h>

class MyThreadAwareHeap {
public:
  // Heap operations (required)
  void* malloc(size_t sz);
  void free(void* ptr);
  void* memalign(size_t align, size_t sz);
  size_t getSize(void* ptr);
  void lock();
  void unlock();

  // Thread hooks (optional - for per-thread TLABs, caches, etc.)
  void threadInit();      // Called when new thread starts
  void threadCleanup();   // Called when thread exits
};

using MyRedirect = alloc8::HeapRedirect<MyThreadAwareHeap>;
ALLOC8_REDIRECT_WITH_THREADS(MyRedirect);

// Or use separate macros for more control:
// ALLOC8_REDIRECT(MyRedirect);
// using MyThreads = alloc8::ThreadRedirect<MyThreadAwareHeap>;
// ALLOC8_THREAD_REDIRECT(MyThreads);
```

CMake:
```cmake
add_library(myalloc SHARED
  my_allocator.cpp
  ${ALLOC8_INTERPOSE_SOURCES}
  ${ALLOC8_THREAD_SOURCES}  # Required for thread hooks
)
target_link_libraries(myalloc PRIVATE alloc8::interpose)
```

### Pattern 3: Direct xxmalloc (For Complex Heaps)

For complex allocators like Hoard that need full control, implement the xxmalloc functions directly.

```cpp
#include <alloc8/platform.h>

extern "C" {
  ALLOC8_EXPORT void* xxmalloc(size_t sz) { ... }
  ALLOC8_EXPORT void xxfree(void* ptr) { ... }
  ALLOC8_EXPORT void* xxmemalign(size_t alignment, size_t sz) { ... }
  ALLOC8_EXPORT size_t xxmalloc_usable_size(void* ptr) { ... }
  ALLOC8_EXPORT void xxmalloc_lock() { ... }
  ALLOC8_EXPORT void xxmalloc_unlock() { ... }
  ALLOC8_EXPORT void* xxrealloc(void* ptr, size_t sz) { ... }
  ALLOC8_EXPORT void* xxcalloc(size_t count, size_t sz) { ... }

  // Optional thread hooks
  ALLOC8_EXPORT void xxthread_init() { ... }
  ALLOC8_EXPORT void xxthread_cleanup() { ... }
}
```

### Pattern 4: Header-Only gnu_wrapper.h (For Zero-Overhead on Linux)

For maximum performance on Linux, use the header-only `gnu_wrapper.h` which calls `getCustomHeap()` directly, enabling full inlining with LTO.

```cpp
// my_allocator.cpp
#include "heaplayers.h"  // or your heap implementation

// Define your heap type
class TheCustomHeapType : public MyHeapImplementation {
public:
  void* memalign(size_t alignment, size_t sz) { ... }
  void lock() { ... }
  void unlock() { ... }
};

// Provide the getCustomHeap() singleton (Meyers singleton pattern)
inline static TheCustomHeapType* getCustomHeap() {
  static char buf[sizeof(TheCustomHeapType)];
  static TheCustomHeapType* heap = new (buf) TheCustomHeapType;
  return heap;
}

// Include alloc8's header-only wrapper
#include <alloc8/gnu_wrapper.h>
```

CMake (requires GCC 11+ and IPO for best results):
```cmake
add_library(myalloc SHARED my_allocator.cpp)
target_include_directories(myalloc PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Enable IPO/LTO
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported)
if(ipo_supported)
  set_target_properties(myalloc PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

target_compile_options(myalloc PRIVATE
  -fvisibility=hidden
  -fno-builtin-malloc -fno-builtin-free
  -fno-builtin-realloc -fno-builtin-calloc
)

target_link_libraries(myalloc PRIVATE pthread dl)
```

## Testing Interposition

```bash
# Create simple test program
cat > test.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
int main() {
    void* p = malloc(100);
    printf("malloc: %p\n", p);
    free(p);
    return 0;
}
EOF
clang test.c -o test

# Test on macOS
DYLD_INSERT_LIBRARIES=./libmyalloc.dylib ./test

# Test on Linux
LD_PRELOAD=./libmyalloc.so ./test
```

## CMake Variables for Users

When building an allocator library with alloc8:
- `${ALLOC8_INTERPOSE_SOURCES}` - Platform-specific interposition source files
- `${ALLOC8_THREAD_SOURCES}` - Thread lifecycle hooks (for thread-aware allocators)
- `${ALLOC8_COMMON_SOURCES}` - Common wrapper implementations (not needed on Linux)
- `alloc8::interpose` - Link target with proper flags and dependencies
- `alloc8::headers` - Header-only interface

## Code Style

- C++20 with concepts
- Use `ALLOC8_*` prefix for all macros
- Platform-specific code isolated in `src/platform/{linux,macos,windows}/`
- Header-only where possible, object libraries for shared code
- Use `ALLOC8_EXPORT` for symbols that must be visible

## File Organization

```
alloc8/
├── include/alloc8/          # Public headers
│   ├── alloc8.h             # Main header + ALLOC8_REDIRECT macros
│   ├── allocator_traits.h   # HeapRedirect<T>, ThreadRedirect<T> templates
│   ├── platform.h           # Platform detection macros
│   ├── thread_hooks.h       # Thread lifecycle hooks documentation
│   └── gnu_wrapper.h        # Header-only Linux wrapper (zero-overhead)
├── src/
│   ├── common/              # Shared wrapper implementations
│   │   ├── wrapper_common.cpp
│   │   ├── new_delete.cpp
│   │   └── new_delete.inc   # Included by platform wrappers
│   └── platform/
│       ├── linux/
│       │   ├── gnu_wrapper.cpp     # Main Linux interposition
│       │   └── linux_threads.cpp   # Thread lifecycle hooks
│       ├── macos/
│       │   ├── mac_wrapper.cpp     # Includes mac_zones.cpp
│       │   └── mac_threads.cpp     # Thread lifecycle hooks
│       └── windows/win_wrapper_detours.cpp
├── examples/
│   ├── simple_heap/         # Basic example with statistics
│   ├── diehard/             # DieHard integration (working)
│   └── hoard/               # Hoard integration (working on Linux)
└── tests/
```

## Debugging Tips

1. **Check interpose section exists (macOS):**
   ```bash
   otool -l libmyalloc.dylib | grep -A5 "__interpose"
   ```

2. **Check exported symbols:**
   ```bash
   # macOS
   nm -gU libmyalloc.dylib | grep malloc
   # Linux
   nm -D libmyalloc.so | grep malloc
   ```

3. **Debug with lldb/gdb:**
   ```bash
   # macOS
   DYLD_INSERT_LIBRARIES=./libmyalloc.dylib lldb ./test
   # Linux
   LD_PRELOAD=./libmyalloc.so gdb ./test
   ```

4. **Check if getCustomHeap is inlined (Linux with LTO):**
   ```bash
   nm -C libmyalloc.so | grep getCustomHeap
   # If fully inlined, only the guard variable should appear
   ```

5. **If works in debugger but not standalone:** Likely initialization timing issue.

## API Reference

### Macros

| Macro | Description |
|-------|-------------|
| `ALLOC8_REDIRECT(HeapRedirectType)` | Generate xxmalloc functions from HeapRedirect |
| `ALLOC8_THREAD_REDIRECT(ThreadRedirectType)` | Generate xxthread functions from ThreadRedirect |
| `ALLOC8_REDIRECT_WITH_THREADS(HeapRedirectType)` | Combined heap + thread redirect |
| `ALLOC8_EXPORT` | Mark symbol for export |
| `ALLOC8_ALWAYS_INLINE` | Force inlining |
| `ALLOC8_LIKELY(x)` / `ALLOC8_UNLIKELY(x)` | Branch prediction hints |

### Templates

| Template | Description |
|----------|-------------|
| `alloc8::HeapRedirect<T>` | Wraps allocator T, provides static xxmalloc interface |
| `alloc8::ThreadRedirect<T>` | Wraps allocator T, provides static xxthread interface |
| `alloc8::Redirect<T>` | Alias for HeapRedirect<T> |
| `alloc8::Threads<T>` | Alias for ThreadRedirect<T> |

### Concepts (C++20)

| Concept | Required Methods |
|---------|------------------|
| `alloc8::Allocator` | malloc, free, memalign, getSize, lock, unlock |
| `alloc8::AllocatorWithRealloc` | Allocator + realloc |
| `alloc8::ThreadAwareAllocator` | threadInit, threadCleanup |

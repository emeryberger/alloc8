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

The DieHard example achieves **zero overhead** compared to the original DieHard by:

1. **Using Heap-Layers wrappers directly** - Includes `gnuwrapper.cpp` which calls `getCustomHeap()` directly, bypassing the `xxmalloc` indirection
2. **Full LTO with GCC 11+** - Uses `-flto=auto` and `-fno-fat-lto-objects` for aggressive inlining
3. **C++23 standard** - Enables latest optimizations
4. **Identical architecture** - Uses the same Meyers singleton pattern as original DieHard

The `xxmalloc` interface is still provided for compatibility (fork handlers, etc.) but the hot path goes directly through `getCustomHeap()`.

Performance comparison (threadtest, 1 thread, 10000 iterations):
- alloc8 DieHard: ~0.35s
- Original DieHard: ~0.35s (within variance)

## Architecture

### Key Components

1. **`include/alloc8/alloc8.h`** - Main header with `ALLOC8_REDIRECT` macro
2. **`include/alloc8/allocator_traits.h`** - `HeapRedirect<T>` template and `Allocator` concept
3. **`include/alloc8/platform.h`** - Platform detection and compiler attribute macros

### Platform Wrappers

| Platform | File | Mechanism |
|----------|------|-----------|
| Linux | `src/platform/linux/gnu_wrapper.cpp` | Strong symbol aliasing |
| macOS | `src/platform/macos/mac_wrapper.cpp` | `__DATA,__interpose` section |
| Windows | `src/platform/windows/win_wrapper_detours.cpp` | Microsoft Detours |

### xxmalloc Interface

The bridge between platform wrappers and user allocators:
- `xxmalloc(size_t)` - Called by platform wrappers
- `xxfree(void*)` - Free memory
- `xxmemalign(size_t, size_t)` - Aligned allocation
- `xxmalloc_usable_size(void*)` - Get allocation size
- `xxmalloc_lock()` / `xxmalloc_unlock()` - Fork safety
- `xxrealloc(void*, size_t)` - Reallocation
- `xxcalloc(size_t, size_t)` - Zeroed allocation

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
```cpp
class MyHeap {
public:
  void* malloc(size_t sz);
  void free(void* ptr);
  void* memalign(size_t align, size_t sz);
  size_t getSize(void* ptr);
  void lock();
  void unlock();
};

using MyRedirect = alloc8::HeapRedirect<MyHeap>;
ALLOC8_REDIRECT(MyRedirect);
```

### Pattern 2: HeapRedirect with Thread Hooks (For Per-Thread State)
```cpp
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

// Or separately:
// ALLOC8_REDIRECT(MyRedirect);
// using MyThreads = alloc8::ThreadRedirect<MyThreadAwareHeap>;
// ALLOC8_THREAD_REDIRECT(MyThreads);
```

CMake setup for thread-aware allocators:
```cmake
add_library(myalloc SHARED
  my_allocator.cpp
  ${ALLOC8_INTERPOSE_SOURCES}
  ${ALLOC8_THREAD_SOURCES}  # Required for thread hooks
)
target_link_libraries(myalloc PRIVATE alloc8::interpose)
```

### Pattern 3: Direct xxmalloc (For Complex Heaps like Hoard)
```cpp
// Implement xxmalloc functions directly
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
- `${ALLOC8_THREAD_SOURCES}` - Optional thread lifecycle hooks (for thread-aware allocators)
- `${ALLOC8_COMMON_SOURCES}` - Common wrapper object files (calloc, realloc, etc.) - Note: not needed on Linux as gnu_wrapper.cpp includes everything
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
│   ├── alloc8.h             # Main header + ALLOC8_REDIRECT
│   ├── allocator_traits.h   # HeapRedirect<T> template
│   ├── platform.h           # Platform detection macros
│   └── thread_hooks.h       # Thread lifecycle hooks interface
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

1. **Check interpose section exists:**
   ```bash
   otool -l libmyalloc.dylib | grep -A5 "__interpose"
   ```

2. **Check exported symbols:**
   ```bash
   nm -gU libmyalloc.dylib | grep malloc
   ```

3. **Debug with lldb:**
   ```bash
   DYLD_INSERT_LIBRARIES=./libmyalloc.dylib lldb ./test
   ```

4. **If works in debugger but not standalone:** Likely initialization timing issue.

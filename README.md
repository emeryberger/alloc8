# alloc8

A generic, platform-independent library for replacing system allocators with custom implementations.

Factored from proven patterns in [Hoard](https://github.com/emeryberger/Hoard), [DieHard](https://github.com/emeryberger/DieHard), and [Scalene](https://github.com/plasma-umass/scalene).

## Features

- **Platform Independent**: Linux, macOS, and Windows support
- **CMake Integration**: Easy to vendor via `FetchContent`
- **Two Modes**:
  - **Interposer**: Replace system malloc via `LD_PRELOAD` / `DYLD_INSERT_LIBRARIES` / DLL injection
  - **Prefixed**: Standalone `prefix_malloc()` functions alongside system malloc
- **C++ Templates**: Zero-overhead abstraction via `HeapRedirect<T>`
- **Full C++ Support**: Replaces `operator new` / `delete` including C++17 aligned variants

## Quick Start

### 1. Add alloc8 to your project

```cmake
include(FetchContent)
FetchContent_Declare(
  alloc8
  GIT_REPOSITORY https://github.com/yourusername/alloc8.git
  GIT_TAG        v1.0.0
)
FetchContent_MakeAvailable(alloc8)
```

### 2. Define your allocator

```cpp
// my_allocator.cpp
#include <alloc8/alloc8.h>
#include <sys/mman.h>  // mmap/munmap

class MyHeap {
public:
  void* malloc(size_t sz) {
    // IMPORTANT: Do NOT call malloc/free here - use mmap or your own logic.
    // Calling malloc would cause infinite recursion under LD_PRELOAD.
    size_t total = sz + sizeof(size_t);
    void* p = mmap(nullptr, total, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    *(size_t*)p = total;  // store size for free/getSize
    return (char*)p + sizeof(size_t);
  }

  void free(void* ptr) {
    if (!ptr) return;
    void* base = (char*)ptr - sizeof(size_t);
    size_t total = *(size_t*)base;
    munmap(base, total);
  }

  void* memalign(size_t alignment, size_t sz) {
    // Simplified: mmap returns page-aligned memory
    return malloc(sz);
  }

  size_t getSize(void* ptr) {
    if (!ptr) return 0;
    void* base = (char*)ptr - sizeof(size_t);
    return *(size_t*)base - sizeof(size_t);
  }

  void lock() { /* for fork safety */ }
  void unlock() { /* for fork safety */ }
};

// Generate xxmalloc interface
using MyRedirect = alloc8::HeapRedirect<MyHeap>;
ALLOC8_REDIRECT(MyRedirect);
```

### 3. Build as shared library

```cmake
add_library(myalloc SHARED
  my_allocator.cpp
  ${ALLOC8_INTERPOSE_SOURCES}
  ${ALLOC8_COMMON_SOURCES}
)
target_link_libraries(myalloc PRIVATE alloc8::interpose)
```

### 4. Use with LD_PRELOAD

```bash
# Linux
LD_PRELOAD=./libmyalloc.so ./my_program

# macOS
DYLD_INSERT_LIBRARIES=./libmyalloc.dylib ./my_program
```

## Prefixed Mode

For allocators that should coexist with system malloc:

```cmake
set(ALLOC8_PREFIX "myalloc")
FetchContent_MakeAvailable(alloc8)

add_library(myalloc_api STATIC my_allocator.cpp)
target_link_libraries(myalloc_api PRIVATE alloc8::prefixed)
```

This generates `myalloc_malloc()`, `myalloc_free()`, etc.

## Allocator Requirements

Your allocator class must implement:

| Method | Description |
|--------|-------------|
| `void* malloc(size_t sz)` | Allocate memory |
| `void free(void* ptr)` | Free memory |
| `void* memalign(size_t align, size_t sz)` | Aligned allocation |
| `size_t getSize(void* ptr)` | Get usable size |
| `void lock()` | Lock for fork safety |
| `void unlock()` | Unlock for fork safety |

Optional: `void* realloc(void* ptr, size_t sz)` - if not provided, a default implementation is used.

## Building alloc8

To build with tests and examples:

```bash
mkdir build && cd build
cmake .. -DALLOC8_BUILD_TESTS=ON -DALLOC8_BUILD_EXAMPLES=ON
cmake --build .
ctest
```

## Examples

### SimpleHeap

The `examples/simple_heap` directory contains a complete example allocator that wraps system malloc with statistics tracking:

```bash
# Build and test (macOS example)
cd build
DYLD_INSERT_LIBRARIES=./examples/simple_heap/libsimple_heap.dylib /bin/ls

# Output at exit:
# === SimpleHeap Statistics ===
# Total allocated: 535366 bytes
# ...
```

### DieHard

The `examples/diehard` directory shows how to integrate [DieHard](https://github.com/emeryberger/DieHard), a memory allocator that provides probabilistic memory safety. DieHard and Heap-Layers are automatically fetched via CMake FetchContent.

**Build:**
```bash
cmake .. -DALLOC8_BUILD_EXAMPLES=ON -DALLOC8_BUILD_DIEHARD_EXAMPLE=ON
cmake --build .
```

**Use:**
```bash
# Linux
LD_PRELOAD=./examples/diehard/libdiehard_alloc8.so ./my_program

# macOS
DYLD_INSERT_LIBRARIES=./examples/diehard/libdiehard_alloc8.dylib ./my_program
```

### Hoard

The `examples/hoard` directory shows how to integrate [Hoard](https://github.com/emeryberger/Hoard), a fast, scalable memory allocator. Hoard and Heap-Layers are automatically fetched via CMake FetchContent.

**Build:**
```bash
cmake .. -DALLOC8_BUILD_EXAMPLES=ON -DALLOC8_BUILD_HOARD_EXAMPLE=ON
cmake --build .
```

**Note:** The Hoard example currently has initialization timing issues on macOS. The DieHard example demonstrates the same integration pattern and works correctly.

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ALLOC8_BUILD_TESTS` | OFF | Build test suite |
| `ALLOC8_BUILD_EXAMPLES` | OFF | Build example allocators |
| `ALLOC8_BUILD_HOARD_EXAMPLE` | OFF | Build Hoard integration example |
| `ALLOC8_BUILD_DIEHARD_EXAMPLE` | OFF | Build DieHard integration example |
| `ALLOC8_PREFIX` | "" | Prefix for prefixed mode (e.g., "hoard" → `hoard_malloc`) |
| `ALLOC8_WINDOWS_USE_DETOURS` | ON | Use Microsoft Detours on Windows |

## Platform Details

### Linux
- Uses strong symbol aliasing via `__attribute__((alias(...)))`
- Version script for GLIBC compatibility
- Requires `-Bsymbolic` linker flag

### macOS
- Uses `__DATA,__interpose` Mach-O section
- Full `malloc_zone_t` implementation
- Fork safety via `_malloc_fork_*` interposition

### Windows
- Uses [Microsoft Detours](https://github.com/microsoft/Detours)
- Patches CRT modules dynamically
- Handles "foreign" pointers from pre-hook allocations

## License

Apache 2.0 - see LICENSE file.

## Acknowledgments

Based on allocator interposition patterns from:
- [Heap-Layers](https://github.com/emeryberger/Heap-Layers) by Emery Berger
- [Hoard](https://github.com/emeryberger/Hoard) by Emery Berger
- [DieHard](https://github.com/emeryberger/DieHard) by Emery Berger
- [Scalene](https://github.com/plasma-umass/scalene) by Emery Berger et al.

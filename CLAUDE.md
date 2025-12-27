# CLAUDE.md - AI Assistant Guide for alloc8

## Project Overview

alloc8 is a platform-independent allocator interposition library. It provides the infrastructure to replace system malloc/free with custom allocators via LD_PRELOAD (Linux), DYLD_INSERT_LIBRARIES (macOS), or Detours (Windows).

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
```

## Building DieHard/Hoard Examples

```bash
# DieHard example (working)
cmake -B build \
  -DALLOC8_BUILD_EXAMPLES=ON \
  -DALLOC8_BUILD_DIEHARD_EXAMPLE=ON \
  -DDIEHARD_SOURCE_DIR=~/git/DieHard \
  -DHEAPLAYERS_SOURCE_DIR=~/git/Heap-Layers
cmake --build build

# Test DieHard
DYLD_INSERT_LIBRARIES=build/examples/diehard/libdiehard_alloc8.dylib ./test_program

# Hoard example (has macOS init timing issues)
cmake -B build \
  -DALLOC8_BUILD_EXAMPLES=ON \
  -DALLOC8_BUILD_HOARD_EXAMPLE=ON \
  -DHOARD_SOURCE_DIR=~/git/Hoard \
  -DHEAPLAYERS_SOURCE_DIR=~/git/Heap-Layers
cmake --build build
```

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

## Important Implementation Details

### macOS Specifics
- `mac_zones.cpp` is `#included` by `mac_wrapper.cpp`, not compiled separately
- macOS does NOT have `memalign()` - only `posix_memalign()`
- ARM64 macOS uses 16KB pages (`ALLOC8_PAGE_SIZE`)
- Full malloc_zone_t implementation enables interposition without DYLD_FORCE_FLAT_NAMESPACE

### Linux Specifics
- Uses version script (`version_script.map`) for GLIBC symbol versioning
- Requires `-Bsymbolic` linker flag to avoid infinite recursion
- Compiler flags: `-fno-builtin-malloc`, `-fno-builtin-free`, etc.

### Windows Specifics
- Microsoft Detours fetched via CMake FetchContent
- Must handle "foreign" pointers allocated before hooks installed
- Uses SEH for safe foreign pointer detection

## Common Issues

1. **Infinite recursion**: Allocator calling malloc internally -> use raw mmap/VirtualAlloc
2. **Missing `ALLOC8_COMMON_SOURCES`**: CMake users must include this in their target
3. **macOS zone APIs**: Must implement full `malloc_zone_t` for compatibility
4. **C++ operator placement**: Use `__builtin_expect` not `[[likely]]` in macro contexts

## Testing

```bash
# Native tests (without interposition)
./build/tests/test_basic_alloc

# Interposition test (macOS)
DYLD_INSERT_LIBRARIES=./build/examples/simple_heap/libsimple_heap.dylib ./build/test_interpose
```

## Code Style

- C++20 with concepts
- Use `ALLOC8_*` prefix for all macros
- Platform-specific code isolated in `src/platform/{linux,macos,windows}/`
- Header-only where possible, object libraries for shared code

# alloc8 TODO

Status and plans for the alloc8 allocator interposition library.

## Current Status

### Implemented

| Feature | Linux | macOS | Windows |
|---------|-------|-------|---------|
| Basic malloc/free interposition | Done | Done | Done |
| calloc/realloc | Done | Done | Done |
| memalign/posix_memalign/aligned_alloc | Done | Done | Done |
| valloc/pvalloc | Done | Done | N/A |
| malloc_usable_size | Done | Done | Done |
| C++ operator new/delete | Done | Done | Done |
| C++17 aligned new/delete | Done | Done | Done |
| malloc_zone_t support | N/A | Done | N/A |
| Prefixed mode | Done | Done | Done |
| Thread lifecycle hooks | Done | Done | Planned |
| ThreadRedirect template | Done | Done | Done |
| Header-only gnu_wrapper.h | Done | N/A | N/A |

### Examples

| Example | Status | Notes |
|---------|--------|-------|
| simple_heap | Working | Basic mmap-based allocator with stats |
| DieHard | Working | Zero-overhead with gnu_wrapper.h + LTO |
| Hoard | Working | Uses alloc8 thread hooks for TLAB support |

## Roadmap

### High Priority

- [x] **Linux thread hooks** (`src/platform/linux/linux_threads.cpp`)
  - Implement pthread_create/pthread_exit interposition for Linux
  - Pattern similar to macOS mac_threads.cpp
  - Uses dlsym(RTLD_NEXT) and strong symbol aliasing

- [x] **ThreadRedirect template** (`include/alloc8/allocator_traits.h`)
  - Add `ThreadRedirect<T>` template mirroring `HeapRedirect<T>`
  - Add `ALLOC8_THREAD_REDIRECT` macro
  - Add `ALLOC8_REDIRECT_WITH_THREADS` combined macro
  - Add `ThreadAwareAllocator` concept

- [x] **Header-only gnu_wrapper.h** (`include/alloc8/gnu_wrapper.h`)
  - Zero-overhead Linux interposition via getCustomHeap() pattern
  - Enables full inlining with LTO
  - Used by DieHard example for parity with original

- [ ] **Windows thread hooks** (`src/platform/windows/win_threads.cpp`)
  - Hook CreateThread/ExitThread via Detours
  - Support both native threads and pthread-win32

- [ ] **Test suite expansion**
  - Thread safety stress tests
  - Fork safety tests
  - Alignment edge cases
  - Large allocation tests

### Medium Priority

- [ ] **Scalene integration example**
  - Demonstrate sampling allocator pattern
  - Show how to integrate with Python

- [ ] **Documentation improvements**
  - API reference documentation
  - Platform-specific gotchas
  - Debugging guide for interposition issues

- [ ] **CI/CD setup**
  - GitHub Actions for Linux/macOS/Windows
  - Automated testing with LD_PRELOAD/DYLD_INSERT_LIBRARIES

### Low Priority

- [ ] **Alternative Windows hooking**
  - MinHook support as Detours alternative
  - IAT patching fallback

- [ ] **musl libc support**
  - Test and document musl compatibility
  - May need different symbol aliasing approach

- [ ] **Android support**
  - Bionic libc compatibility
  - Test with Android NDK

## Known Issues

### macOS

1. **SIP restrictions**: DYLD_INSERT_LIBRARIES doesn't work with system binaries on macOS 10.11+. Users must test with their own executables or disable SIP.

2. **arm64e limitations**: Pointer authentication on Apple Silicon may affect some interposition scenarios.

### Linux

1. **glibc version compatibility**: Version script may need adjustment for older glibc versions.

2. **Static linking**: Interposition doesn't work with statically linked executables.

### Windows

1. **CRT module enumeration**: Foreign pointer detection depends on correctly identifying all CRT modules.

2. **32-bit vs 64-bit**: Detours patterns differ slightly between architectures.

## Design Decisions

### Thread Hook Architecture

The thread lifecycle hooks (`xxthread_init`/`xxthread_cleanup`) use weak symbols so allocators can optionally provide them:

```cpp
// Allocator provides these if it needs thread awareness
extern "C" void xxthread_init(void);      // Called in new thread before user function
extern "C" void xxthread_cleanup(void);   // Called when thread exits
```

Benefits:
- Zero overhead when hooks not provided (weak symbol resolves to nullptr)
- Proper initialization ordering via constructor priorities
- Allocator doesn't need platform-specific pthread code

### Constructor Priority Ordering

```
Priority 101: Allocator initialization (xxmalloc ready)
Priority 200: Thread hooks activation (pthread interposition enabled)
Priority 300+: User code can safely use threads
```

This ensures malloc is ready before pthread interposition activates, preventing the SIGBUS crashes that occurred with naive interposition.

## Contributing

Contributions welcome! Key areas:
- Linux/Windows thread hook implementations
- Additional allocator examples
- Test coverage improvements
- Documentation

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
  GIT_REPOSITORY https://github.com/emeryberger/alloc8.git
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

### 4. Use with LD_PRELOAD / Detours

```bash
# Linux
LD_PRELOAD=./libmyalloc.so ./my_program

# macOS
DYLD_INSERT_LIBRARIES=./libmyalloc.dylib ./my_program

# Windows - DLL is loaded and hooks installed automatically via DllMain
# Copy myalloc.dll to your application directory, or use withdll.exe from Detours
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

## Thread-Aware Allocators (Optional)

High-performance allocators like Hoard use per-thread heaps (TLABs) to reduce contention. alloc8 provides optional pthread interposition to support these allocators with proper initialization ordering.

### Recommended: ThreadRedirect Template

The easiest way to add thread hooks is to add `threadInit()` and `threadCleanup()` methods to your heap class:

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

  // Thread hooks (optional)
  void threadInit() {
    // Initialize per-thread heap structures (TLABs)
  }

  void threadCleanup() {
    // Flush thread-local allocation buffers
  }
};

using MyRedirect = alloc8::HeapRedirect<MyThreadAwareHeap>;
ALLOC8_REDIRECT_WITH_THREADS(MyRedirect);

// Or use separate macros:
// ALLOC8_REDIRECT(MyRedirect);
// using MyThreads = alloc8::ThreadRedirect<MyThreadAwareHeap>;
// ALLOC8_THREAD_REDIRECT(MyThreads);
```

### Alternative: Direct xxthread Functions

For more control, implement the hooks directly:

```cpp
extern "C" {
  void xxthread_init(void) {
    // Initialize per-thread heap structures (TLABs)
  }

  void xxthread_cleanup(void) {
    // Flush thread-local allocation buffers
  }

  // Optional: Flag for single-threaded lock optimization
  volatile int xxthread_created_flag;
}
```

### CMake Integration

Include `${ALLOC8_THREAD_SOURCES}` in your library:

```cmake
add_library(myalloc SHARED
  my_allocator.cpp
  ${ALLOC8_INTERPOSE_SOURCES}
  ${ALLOC8_THREAD_SOURCES}  # Enables pthread interposition
)
target_link_libraries(myalloc PRIVATE alloc8::interpose)
```

### How It Works

1. alloc8 interposes `pthread_create` and `pthread_exit`
2. When a thread is created, alloc8 wraps the thread function
3. `xxthread_init()` is called in the new thread before the user function runs
4. `xxthread_cleanup()` is called when the thread exits
5. Weak symbol detection: if hooks aren't provided, pthread calls pass through with zero overhead

### Benefits

- **Proper Initialization Ordering**: alloc8 ensures pthread hooks activate after malloc is fully ready, avoiding crashes during early library initialization
- **Platform Abstraction**: Allocators don't need platform-specific pthread interposition code
- **Zero Overhead When Unused**: If you don't provide hooks, pthread calls pass through directly

See the Hoard example for a complete implementation using thread hooks.

## Ownership Hook (Optional, macOS)

Allocators whose memory lives in fixed regions (never unmapped) can provide an
`xxowns(ptr)` hook to bypass alloc8's per-allocation hash table on the
free/realloc/malloc_size hot paths. This eliminates one hash-table
insert+lookup+delete per allocation cycle.

```cpp
// In your allocator .cpp, alongside ALLOC8_REDIRECT:
extern "C" bool xxowns_active() { return true; }

extern "C" bool xxowns(const void* ptr) {
    // Return true iff ptr was returned by your allocator.
    // Must be safe on ANY address (including libSystem/libobjc pointers).
    return my_heap_region_contains(ptr);
}
```

When `xxowns_active()` returns true, alloc8 skips `mark_owned`/`forget_size`
bookkeeping and uses `xxowns()` as the sole ownership predicate. When not
provided, weak default definitions (return false) preserve the existing
hash-table behavior with no overhead.

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

Optional methods:
| Method | Description |
|--------|-------------|
| `void* realloc(void* ptr, size_t sz)` | Reallocation (default provided) |
| `void threadInit()` | Called when new thread starts |
| `void threadCleanup()` | Called when thread exits |
| `bool xxowns(const void* ptr)` | Ownership predicate (macOS, skips hash table) |
| `bool xxowns_active()` | Return true to enable xxowns hook |

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
# Unix
cmake .. -DALLOC8_BUILD_EXAMPLES=ON -DALLOC8_BUILD_DIEHARD_EXAMPLE=ON
cmake --build .

# Windows
cmake .. -DALLOC8_BUILD_EXAMPLES=ON -DALLOC8_BUILD_DIEHARD_EXAMPLE=ON
cmake --build . --config Release
```

**Use:**
```bash
# Linux
LD_PRELOAD=./examples/diehard/libdiehard_alloc8.so ./my_program

# macOS
DYLD_INSERT_LIBRARIES=./examples/diehard/libdiehard_alloc8.dylib ./my_program

# Windows - output: examples/diehard/Release/diehard_alloc8.dll
```

### Hoard

The `examples/hoard` directory shows how to integrate [Hoard](https://github.com/emeryberger/Hoard), a fast, scalable memory allocator. Hoard and Heap-Layers are automatically fetched via CMake FetchContent.

**Build:**
```bash
# Unix
cmake .. -DALLOC8_BUILD_EXAMPLES=ON -DALLOC8_BUILD_HOARD_EXAMPLE=ON
cmake --build .

# Windows
cmake .. -DALLOC8_BUILD_EXAMPLES=ON -DALLOC8_BUILD_HOARD_EXAMPLE=ON
cmake --build . --config Release
```

**Use:**
```bash
# Linux
LD_PRELOAD=./examples/hoard/libhoard_alloc8.so ./my_program

# macOS (has timing issues - use Linux or Windows)
DYLD_INSERT_LIBRARIES=./examples/hoard/libhoard_alloc8.dylib ./my_program

# Windows - output: examples/hoard/Release/hoard_alloc8.dll
```

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
- Foreign-pointer routing — see [macOS Compatibility (Firefox / GUI apps)](#macos-compatibility-firefox--gui-apps).

## macOS Compatibility (Firefox / GUI apps)

Modern macOS GUI applications (Firefox, Chromium, anything that links libobjc /
CoreFoundation / libxpc / libsandbox) hit two failure modes when an allocator
is injected via `DYLD_INSERT_LIBRARIES`:

1. **`malloc_size`-based pointer validation.** libobjc's class-realization
   path runs `ASSERT(malloc_size(cls) >= sizeof(objc_class))` for every class
   it touches and aborts with `realized class … has corrupt data pointer:
   malloc_size(…) = 0` on the first hit if the answer is wrong. CoreFoundation
   and libxpc do similar pointer probes through `malloc_zone_from_ptr` and
   `zone->size`. A user allocator that returns 0 (DieHard) or dereferences
   garbage (Hoard) on a foreign pointer crashes the process there.
2. **Child-process sandbox compilation.** Firefox spawns helpers
   (`plugin-container`, `Nightly GPU Helper`) which compile sandbox profiles
   via libsandbox's TinyScheme parser during very-early dyld init. If our
   interposers are present at that moment, the parser's free path picks up
   the wrong zone and aborts with
   `BUG IN CLIENT OF LIBMALLOC: POINTER BEING FREED WAS NOT ALLOCATED`.

### What alloc8 does on macOS

- **Foreign-pointer routing.** Every `replace_*` entry point checks an
  ownership table before touching the user allocator. If the pointer was
  issued by us, we forward to `xxmalloc_usable_size`/`xxfree`/etc; otherwise
  we route to the libSystem zone that owns it (resolved via the captured
  original `malloc_zone_from_ptr` — see below). Pointers in no zone we
  recognize (dyld shared cache, third-party mmaps) are dropped silently
  rather than handed to libSystem free, which would abort.
- **Sharded ownership/size table.** A 256-shard × 16,384-bucket × 64-probe
  open-addressed hash table records every pointer the user allocator returns
  and its requested size. Inserts and lookups are lock-free CAS on a single
  shard; sharding by the high bits of the splitmix hash keeps cache-line
  contention low across many threads. Lazily mmap'd, ~64 MB virtual /
  ~physical-pages-touched.
- **Original-libSystem function recovery.** `dlsym(…, "malloc_zone_from_ptr")`
  returns our wrapper after dyld processes our `__interpose` section; we
  recover the real libSystem pointer by walking our own `__DATA_CONST,__interpose`
  section (also `__AUTH_CONST` on arm64e, `__DATA` on older toolchains)
  and reading the unmodified `original` field.
- **Minimal zone-API surface.** Only `malloc_zone_from_ptr` is interposed at
  the zone API level, and that interposer is a transparent passthrough to
  the captured libSystem implementation. We deliberately do **not** interpose
  `malloc_default_zone`, `malloc_create_zone`, `malloc_get_all_zones`,
  `malloc_zone_register`, or any of the per-zone alloc/free entry points;
  doing so corrupts libsandbox's zone bookkeeping in child processes.
- **`DYLD_INSERT_LIBRARIES` strip-for-children.** A constructor strips
  `DYLD_INSERT_LIBRARIES` from the parent's environment, so spawned helpers
  run with libSystem only. The parent process remains fully instrumented.
  Override with `ALLOC8_NO_STRIP=1` if you want injection in children too;
  alloc8 will then auto-detect known Firefox child-process names
  (`plugin-container`, `Nightly GPU Helper`, `Nightly Media Plugin Helper`,
  `Nightly Security Module Helper`) and run them in passthrough mode (every
  `replace_*` calls libSystem directly, leaving the user allocator out of
  the child).

### Verified

Tested against a self-built Firefox 152.0a1 (macOS 26.4 / arm64) loading
real pages over HTTPS via marionette. Hoard and the simple_heap example
allocator both run as the parent's allocator with pages rendering
correctly; DieHard hits an unrelated pre-existing bug in its own
`HL::STLAllocator<…, LargeHeap<MmapWrapper>::SourceHeap>` rehash path
during Swift autorelease cleanup that is not related to alloc8.

## Stats Gathering (macOS)

alloc8 can count its own work and dump a histogram of requested sizes,
useful for confirming "is my custom allocator actually getting hit?"
without attaching a debugger.

Set `ALLOC8_STATS=1` and the dylib will dump a stats block to stderr every
200,000 user-allocator `malloc` calls. Output includes:

- per-call counts split into `user` (routed to your allocator) and
  `passthrough` (routed to libSystem, in child processes when
  `ALLOC8_NO_STRIP=1`)
- `free → libSystem` and `free dropped (foreign)` — pointers that arrived
  at our `free` interposer but came from outside our allocator
- a histogram of requested sizes (power-of-2 buckets, 1 byte through
  multi-GB) so you can see the allocation distribution

### Example

```bash
ALLOC8_STATS=1 \
  DYLD_INSERT_LIBRARIES=build/examples/hoard/libhoard_alloc8.dylib \
  /path/to/firefox --no-remote --profile /tmp/p https://example.com/
```

Sample output after browsing a few real pages:

```
=== alloc8 stats (proc=firefox, passthrough=0) ===
  malloc:           1,600,000 user             0 passthrough
  calloc:             400,331 user             0 passthrough
  realloc:            132,577 user             0 passthrough
  posix_memalign:         963 user             0 passthrough
  free:             1,609,797 user             0 passthrough
  free → libSystem:     6,577  free dropped (foreign): 452
  malloc_size:         89,972 user           103 libSystem    0 passthrough
  --------------------------------------------------
  total user-allocator calls:     3,833,640
  total passthrough calls:        0
  --------------------------------------------------
  alloc-size histogram (request size at malloc/calloc/realloc/memalign):
            ≤ 1 B          2,824    0.1%
       2 B –    3 B         6,406    0.3%
       4 B –    7 B        18,315    0.9%
       8 B –   15 B       103,579    4.9%  #
      16 B –   31 B       434,713   20.4%  ########
      32 B –   63 B       571,181   26.8%  ##########
      64 B –  127 B       451,012   21.1%  ########
     128 B –  255 B       278,235   13.0%  #####
     256 B –  511 B       135,526    6.4%  ##
     512 B – 1023 B        52,364    2.5%
       1 KB –    1 KB        30,781    1.4%
       2 KB –    3 KB        15,083    0.7%
       4 KB –    7 KB        18,623    0.9%
       8 KB –   15 KB        10,421    0.5%
      16 KB –   31 KB         2,358    0.1%
      …
  total sized requests: 2,133,870
===================================================
```

`passthrough=0` plus `total passthrough calls: 0` confirms every allocation
went to your user allocator (Hoard, here). Anything non-zero in the
passthrough columns means alloc8 routed that call to libSystem instead.

When `ALLOC8_STATS` is unset, the counters are gated by a single relaxed
atomic load per call and never written, so leaving the support compiled in
is free.

## Environment variables (macOS)

| Variable                   | Effect                                                                                                                          |
|----------------------------|----------------------------------------------------------------------------------------------------------------------------------|
| `ALLOC8_STATS=1`           | Enable per-call counters and the size histogram. Dumps to stderr every 200,000 user-allocator `malloc` calls.                  |
| `ALLOC8_DEBUG=1`           | One-line diagnostic at init: which proc, whether passthrough is on, libSystem `malloc_zone_from_ptr` resolution result.        |
| `ALLOC8_PASSTHROUGH=1`     | Force passthrough mode in this process — every `replace_*` calls libSystem instead of the user allocator. `=0` forces it off. |
| `ALLOC8_NO_STRIP=1`        | Don't strip `DYLD_INSERT_LIBRARIES` from the parent's environment. Children will then load alloc8 (and may hit sandbox aborts; alloc8 falls back to passthrough mode for known Firefox child names but other apps may still need work). |

### Windows
- Uses [Microsoft Detours](https://github.com/microsoft/Detours) (auto-fetched via CMake)
- Patches CRT modules dynamically via `DetourEnumerateModules`
- Handles "foreign" pointers from pre-hook allocations
- Thread hooks via `DllMain` `DLL_THREAD_ATTACH`/`DLL_THREAD_DETACH`
- Supports ARM64 and x64 architectures
- Define `ALLOC8_NO_DLLMAIN` to provide custom DllMain

## License

Apache 2.0 - see LICENSE file.

## Acknowledgments

Based on allocator interposition patterns from:
- [Heap-Layers](https://github.com/emeryberger/Heap-Layers) by Emery Berger
- [Hoard](https://github.com/emeryberger/Hoard) by Emery Berger
- [DieHard](https://github.com/emeryberger/DieHard) by Emery Berger
- [Scalene](https://github.com/plasma-umass/scalene) by Emery Berger et al.

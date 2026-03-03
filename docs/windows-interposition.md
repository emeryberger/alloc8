# Windows Interposition Mechanisms

This document covers the different approaches for malloc/free interposition on Windows, their performance characteristics, and implementation details.

## Overview

Unlike Linux (`LD_PRELOAD`) and macOS (`DYLD_INSERT_LIBRARIES`), Windows doesn't have a built-in mechanism for library interposition. Three main approaches exist:

1. **Microsoft Detours** - Inline function hooking via trampolines
2. **IAT Patching** - Import Address Table modification
3. **Early-load Redirect** - IAT patching at load time (mimalloc-redirect style)

## Performance Comparison

### Single-threaded Benchmark (10M allocations)

| Method | Time | Per-alloc | Overhead |
|--------|------|-----------|----------|
| Native malloc | 0.207s | 20.7ns | baseline |
| Detours (inline hook) | 0.196s | 19.6ns | ~11ns/call |
| **alloc8-redirect (IAT)** | **0.082s** | **8.2ns** | **~0ns/call** |

### Key Finding

**alloc8-redirect is 2.4x faster than Detours** for the hooking mechanism overhead.

## Mechanism Details

### 1. Microsoft Detours (Inline Hooking)

Detours patches the first few bytes of the target function with a JMP to your hook:

```
Original:               Patched:
malloc:                 malloc:
  push rbp               jmp hook_malloc    ; 5-14 bytes
  mov rbp, rsp           nop nop nop...
  ...                    ...

                        trampoline:
                          push rbp           ; original bytes
                          mov rbp, rsp
                          jmp malloc+14      ; continue original
```

**Pros:**
- Catches ALL calls (including intra-module calls)
- Well-tested, production-quality library
- Supports attach/detach at runtime

**Cons:**
- ~11ns overhead per call (trampoline JMP)
- Requires careful thread synchronization during attach
- "Foreign pointer" problem - allocations before hooks install

### 2. IAT Patching

Modifies the Import Address Table pointers in loaded modules:

```
Before:                  After:
IAT["malloc"] -> ucrt    IAT["malloc"] -> hook_malloc
```

**Pros:**
- Zero per-call overhead (direct function pointer)
- Simple implementation
- No code modification

**Cons:**
- Only catches cross-module calls via IAT
- Internal CRT calls may bypass hooks
- Must patch all loaded modules

### 3. Early-load Redirect (alloc8-redirect)

Combines IAT patching with early loading to avoid foreign pointer issues:

```
Load order:
1. alloc8-redirect.dll loads (as dependency)
2. DllMain patches IAT before any allocations
3. Main allocator DLL registers its functions
4. All subsequent allocations go through hooks
```

**Pros:**
- Zero per-call overhead
- No foreign pointer problem (patches before allocations)
- 2.4x faster than Detours

**Cons:**
- Requires specific DLL load order
- Only catches IAT calls (like regular IAT patching)

## Implementation: alloc8-redirect

### Architecture

```
┌─────────────────┐
│ Your Program    │
└────────┬────────┘
         │ LoadLibrary / import
         ▼
┌─────────────────┐     depends on     ┌────────────────────┐
│ your_alloc.dll  │ ──────────────────►│ alloc8-redirect.dll│
└────────┬────────┘                    └─────────┬──────────┘
         │                                       │
         │ alloc8_redirect_init()               │ DllMain patches IAT
         │ alloc8_redirect_enable()             │ before any allocs
         ▼                                       ▼
┌─────────────────────────────────────────────────────────────┐
│ All malloc/free calls redirected to your allocator          │
└─────────────────────────────────────────────────────────────┘
```

### Key APIs

```cpp
// Called by your allocator DLL to register functions
void alloc8_redirect_init(
  void* (*xxmalloc)(size_t),
  void (*xxfree)(void*),
  void* (*xxcalloc)(size_t, size_t),
  void* (*xxrealloc)(void*, size_t),
  size_t (*xxmalloc_usable_size)(void*)
);

// Enable/disable redirection
void alloc8_redirect_enable();
void alloc8_redirect_disable();
```

### Implementation Details

The redirect DLL uses undocumented but stable ntdll APIs:

```cpp
// Get PEB to enumerate loaded modules
PEB* peb = NtCurrentTeb()->ProcessEnvironmentBlock;
LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;

// For each module, patch IAT entries
for (each module in list) {
  PatchIATEntry(module, "ucrtbase.dll", "malloc", hooked_malloc);
  PatchIATEntry(module, "ucrtbase.dll", "free", hooked_free);
  // ... etc
}

// Use NtProtectVirtualMemory for memory protection changes
NtProtectVirtualMemory(process, &addr, &size, PAGE_READWRITE, &oldProt);
```

### CRT DLLs to Patch

```cpp
const char* crtDlls[] = {
  "ucrtbase.dll",           // Universal CRT
  "ucrtbased.dll",          // Debug Universal CRT
  "api-ms-win-crt-heap-l1-1-0.dll",  // API set
  "msvcrt.dll",             // Legacy CRT
  nullptr
};
```

## Foreign Pointer Problem

When hooks are installed after program start, some allocations may have already occurred using the native allocator. Freeing these "foreign pointers" through your allocator causes crashes.

### Solutions

1. **Early-load redirect** - Patch before any allocations (best)
2. **Pointer validation** - Check if pointer belongs to your heap
3. **SEH fallback** - Try your free, catch exception, use original

```cpp
// Pointer validation approach (used by mimalloc)
void hooked_free(void* ptr) {
  if (ptr && mi_is_in_heap_region(ptr)) {
    mi_free(ptr);
  } else if (ptr && Real_free) {
    Real_free(ptr);  // Foreign pointer
  }
}
```

## Comparison with mimalloc-redirect

mimalloc-redirect uses a similar approach but with closed-source implementation:

| Feature | alloc8-redirect | mimalloc-redirect |
|---------|-----------------|-------------------|
| Source | Open | Closed (prebuilt binaries) |
| Mechanism | IAT patching | IAT patching (likely) |
| ntdll APIs | NtProtectVirtualMemory | Similar + LdrFindEntryForAddress |
| Architectures | x64, ARM64 | x64, x86, ARM64, ARM64EC |

### Benchmark: mimalloc Allocators

| Method | Time (4 threads) | vs Native |
|--------|------------------|-----------|
| Native malloc | 0.059s | baseline |
| mimalloc-redirect | 0.025s | 2.4x faster |
| mimalloc via Detours | 0.036s | 1.6x faster |

**mimalloc-redirect is ~44% faster than Detours** with the same allocator.

### Benchmark: Hoard Allocator

For complex allocators like Hoard with per-thread caches and TLS lookups, the hooking overhead becomes negligible:

| Method | Time (4 threads, 40000 iter) | Time (8 threads) |
|--------|------------------------------|------------------|
| Native malloc | 0.23s | 0.26s |
| Hoard via Detours | 0.15s | 0.14s |
| Hoard via alloc8-redirect | 0.15s | 0.15s |

**Key insight:** The 2.4x hooking speedup only matters for trivial allocators. For production allocators like Hoard where allocation work dominates, both hooking mechanisms perform identically.

### Benchmark: DieHard Allocator

DieHard with `DIEHARD_SCALABLE=1` uses per-thread heaps via its own TLS management:

| Method | Time (4 threads, 40000 iter) | Time (8 threads) |
|--------|------------------------------|------------------|
| Native malloc | 0.23s | 0.26s |
| DieHard via Detours | 0.30s | 0.28s |
| DieHard via alloc8-redirect | 0.30s | 0.29s |

Same result: both hooking mechanisms perform identically for DieHard.

## MSVC LTO Warning

MSVC's Link-Time Optimization (`/GL` + `/LTCG`) causes a **6x performance regression** on ARM64 Windows for memory allocators. Always disable LTO for allocator DLLs:

```cmake
if(MSVC)
  # Do NOT use /GL - causes 6x slowdown on ARM64
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Ob2 /Oi /Ot")
endif()
```

## Files

- `src/platform/windows/alloc8_redirect.cpp` - Redirect DLL implementation
- `src/platform/windows/win_wrapper_detours.cpp` - Detours-based interposition
- `src/platform/windows/win_wrapper_iat.cpp` - IAT hooking (experimental)
- `examples/hoard/hoard_redirect_win.cpp` - Hoard using redirect mechanism
- `examples/hoard/hoard_thread_hooks_win.cpp` - Hoard using Detours mechanism
- `examples/diehard/diehard_redirect_win.cpp` - DieHard using redirect mechanism
- `tests/redirect_test_dll.cpp` - Test allocator using redirect
- `tests/detours_test_dll.cpp` - Test allocator using Detours

## Recommendations

1. **For simple/trivial allocators**: Use alloc8-redirect for 2.4x faster hooking
2. **For production allocators** (Hoard, mimalloc, etc.): Either mechanism works - allocation overhead dominates
3. **For maximum compatibility**: Use Detours (catches all calls including intra-module)
4. **For existing binaries**: Use Detours with withdll.exe or minject

The 2.4x hooking speedup from alloc8-redirect is only significant when allocation work is trivial (e.g., bump allocators). For production allocators like Hoard where TLS lookups and heap management dominate, both mechanisms perform identically.

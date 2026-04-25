// alloc8/src/platform/macos/mac_wrapper.cpp
// macOS allocator interposition via DYLD_INSERT_LIBRARIES
//
// Reference: Heap-Layers macwrapper.cpp by Emery Berger

#ifndef __APPLE__
#error "This file is for macOS only"
#endif

#include <alloc8/alloc8.h>
#include "mac_interpose.h"

#include <AvailabilityMacros.h>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <pthread.h>

// ─── FORWARD DECLARATIONS ─────────────────────────────────────────────────────

extern "C" {
  void* xxmalloc(size_t);
  void  xxfree(void*);
  void* xxmemalign(size_t, size_t);
  size_t xxmalloc_usable_size(void*);
  void xxmalloc_lock();
  void xxmalloc_unlock();
  void* xxrealloc(void*, size_t);
  void* xxcalloc(size_t, size_t);

  // Functions we interpose on (need declarations for MAC_INTERPOSE)
  void  vfree(void*);
  void _malloc_fork_prepare(void);
  void _malloc_fork_parent(void);
  void _malloc_fork_child(void);

  // C++ operator mangled names
  void* _Znwm(size_t);                    // operator new(size_t)
  void* _Znam(size_t);                    // operator new[](size_t)
  void  _ZdlPv(void*);                    // operator delete(void*)
  void  _ZdaPv(void*);                    // operator delete[](void*)
  void* _ZnwmRKSt9nothrow_t(size_t);      // operator new(size_t, nothrow)
  void* _ZnamRKSt9nothrow_t(size_t);      // operator new[](size_t, nothrow)
  void  _ZdlPvRKSt9nothrow_t(void*);      // operator delete(void*, nothrow)
  void  _ZdaPvRKSt9nothrow_t(void*);      // operator delete[](void*, nothrow)
}

// ─── CORE REPLACEMENT FUNCTIONS ───────────────────────────────────────────────

extern "C" {

// Forward decls for foreign-pointer helpers defined in mac_zones.cpp (which is
// #included below into this translation unit).
static malloc_zone_t* getDefaultZone();
static inline malloc_zone_t* libsystem_zone_for(const void* ptr);
static inline void mark_owned(const void* ptr, size_t sz);
static inline bool maybe_owned(const void* ptr);
static size_t lookup_size(const void* ptr);
static void forget_size(const void* ptr);

// Passthrough mode: in Firefox child processes (plugin-container, GPU helper,
// ...) we route every alloc/free directly to libSystem. The sys_* function
// pointers are captured on the first interposed call, before any allocation
// has had a chance to escape through xxmalloc; system dyld constructors run
// at the default priority and may allocate before our priority-101 init,
// so call-time detection is the only safe spot to do it.
extern bool g_passthrough;
extern void* (*g_sys_malloc)(size_t);
extern void  (*g_sys_free)(void*);
extern void* (*g_sys_calloc)(size_t, size_t);
extern void* (*g_sys_realloc)(void*, size_t);
extern int   (*g_sys_posix_memalign)(void**, size_t, size_t);
extern size_t (*g_sys_malloc_size)(const void*);
extern std::atomic<int> g_init_state;
extern std::atomic<bool> g_stats_enabled;
extern std::atomic<uint64_t> g_count_malloc_user, g_count_malloc_pass;
extern std::atomic<uint64_t> g_count_calloc_user, g_count_calloc_pass;
extern std::atomic<uint64_t> g_count_realloc_user, g_count_realloc_pass;
extern std::atomic<uint64_t> g_count_memalign_user, g_count_memalign_pass;
extern std::atomic<uint64_t> g_count_free_user, g_count_free_pass;
extern std::atomic<uint64_t> g_count_free_libsys, g_count_free_dropped;
extern std::atomic<uint64_t> g_count_size_user, g_count_size_libsys, g_count_size_pass;

static void alloc8_init_once();
static inline void ensure_init() {
  if (g_init_state.load(std::memory_order_acquire) != /*INIT_DONE=*/2) {
    alloc8_init_once();
  }
}
extern "C" void alloc8_dump_stats(int);
extern "C" void alloc8_record_alloc_size(size_t);

static inline void bump(std::atomic<uint64_t>& c) {
  if (!g_stats_enabled.load(std::memory_order_relaxed)) return;
  uint64_t v = c.fetch_add(1, std::memory_order_relaxed) + 1;
  // Dump every N total user-allocator allocations from one chosen counter
  // (g_count_malloc_user is a good proxy for "is the user allocator active").
  // Cheap and signal-free; avoids both atexit/_exit issues and SIGUSR1
  // collisions inside Firefox.
  if (&c == &g_count_malloc_user && (v % 200000) == 0) alloc8_dump_stats(0);
}

// ─── DISPATCH HELPERS ─────────────────────────────────────────────────────────
//
// owned_size(): safe ownership probe. xxmalloc_usable_size is unsafe on
// pointers the user allocator did not return (Hoard/Heap-Layers dereference
// the pointer without first proving ownership). The bitmap caps which
// pointers we hand off; outside an owned slot we return 0.
//
// Some allocators (DieHard CombineHeap with sub-heap dispatch) return 0 from
// getSize on pointers they validly issued. We then fall back to the size
// table populated at allocation time (mac_zones.cpp), which libobjc's
// `malloc_size(p) >= sizeof(objc_class)` validation requires.
static inline size_t owned_size(void* ptr) {
  if (!maybe_owned(ptr)) return 0;
  size_t s = xxmalloc_usable_size(ptr);
  if (s) return s;
  s = lookup_size(ptr);
  return s ? s : 1;
}

void* replace_malloc(size_t sz) {
  ensure_init();
  if (g_passthrough) { bump(g_count_malloc_pass); return g_sys_malloc(sz); }
  bump(g_count_malloc_user);
  alloc8_record_alloc_size(sz);
  void* p = xxmalloc(sz);
  if (p) mark_owned(p, sz ? sz : 1);
  return p;
}

void replace_free(void* ptr) {
  if (!ptr) return;
  ensure_init();
  if (g_passthrough) { bump(g_count_free_pass); g_sys_free(ptr); return; }
  bool ours = maybe_owned(ptr);
  if (ours) {
    bump(g_count_free_user);
    forget_size(ptr);
    xxfree(ptr);
    return;
  }
  // Not in our table — the user allocator never returned this pointer. Hand
  // off to libSystem's zone routing. We CANNOT just call libSystem's free
  // (it would walk the global default zone, which on some macOS versions
  // appears to be ours after dyld interposition), so we route through the
  // captured libSystem malloc_zone_from_ptr to find the owning libSystem
  // zone and call its free callback directly.
  if (malloc_zone_t* z = libsystem_zone_for(ptr)) {
    bump(g_count_free_libsys);
    z->free(z, ptr);
    return;
  }
  // Pointer belongs to no zone we recognize (shared cache, mmap'd by the
  // user without going through us, …). Drop silently — calling libSystem
  // free would abort with "POINTER BEING FREED WAS NOT ALLOCATED".
  bump(g_count_free_dropped);
}

size_t replace_malloc_usable_size(void* ptr) {
  if (!ptr) return 0;
  ensure_init();
  if (g_passthrough) { bump(g_count_size_pass); return g_sys_malloc_size(ptr); }
  size_t s = owned_size(ptr);
  if (s) { bump(g_count_size_user); return s; }
  if (malloc_zone_t* z = libsystem_zone_for(ptr)) {
    bump(g_count_size_libsys);
    return z->size(z, ptr);
  }
  return 0;
}

size_t replace_malloc_good_size(size_t sz) {
  return sz ? sz : 1;
}

void* replace_realloc(void* ptr, size_t sz) {
  ensure_init();
  if (g_passthrough) { bump(g_count_realloc_pass); return g_sys_realloc(ptr, sz); }
  bump(g_count_realloc_user);
  alloc8_record_alloc_size(sz);
  if (!ptr) return replace_malloc(sz);

  // 0 size = free (macOS returns a small allocation)
  if (sz == 0) {
    replace_free(ptr);
    return replace_malloc(1);
  }

  // Ours? Resize in our allocator.
  if (maybe_owned(ptr)) {
    size_t oldSize = owned_size(ptr);
    if ((oldSize / 2 < sz) && (sz <= oldSize)) return ptr;
    void* newPtr = xxmalloc(sz);
    if (newPtr) {
      mark_owned(newPtr, sz);
      size_t copySize = (oldSize < sz) ? oldSize : sz;
      if (copySize) memcpy(newPtr, ptr, copySize);
      forget_size(ptr);
      xxfree(ptr);
    }
    return newPtr;
  }

  // Foreign: route to the libSystem zone that owns it.
  if (malloc_zone_t* z = libsystem_zone_for(ptr)) {
    return z->realloc(z, ptr, sz);
  }

  // Pointer belongs to nobody we recognize — copy into our allocator and
  // leave the original untouched (we can't tell its size or who to free to).
  void* newPtr = xxmalloc(sz);
  if (newPtr) {
    mark_owned(newPtr, sz);
    memcpy(newPtr, ptr, sz);  // best effort: may over-read; same risk libc has.
  }
  return newPtr;
}

// macOS-specific reallocf — same as realloc but always free original on failure.
void* replace_reallocf(void* ptr, size_t sz) {
  void* p = replace_realloc(ptr, sz);
  if (!p && ptr) replace_free(ptr);
  return p;
}

void* replace_calloc(size_t count, size_t size) {
  ensure_init();
  if (g_passthrough) { bump(g_count_calloc_pass); return g_sys_calloc(count, size); }
  bump(g_count_calloc_user);
  alloc8_record_alloc_size(count * size);
  void* p = xxcalloc(count, size);
  if (p) mark_owned(p, count * size);
  return p;
}

char* replace_strdup(const char* s) {
  if (!s) return nullptr;
  size_t len = strlen(s) + 1;
  char* newStr = (char*)xxmalloc(len);
  if (newStr) {
    mark_owned(newStr, len);
    memcpy(newStr, s, len);
  }
  return newStr;
}

void* replace_memalign(size_t alignment, size_t size) {
  void* p = xxmemalign(alignment, size);
  if (p) mark_owned(p, size ? size : 1);
  return p;
}

void* replace_aligned_alloc(size_t alignment, size_t size) {
  if (alignment == 0 || (size % alignment) != 0) {
    return nullptr;
  }
  void* p = xxmemalign(alignment, size);
  if (p) mark_owned(p, size);
  return p;
}

int replace_posix_memalign(void** memptr, size_t alignment, size_t size) {
  ensure_init();
  if (g_passthrough) { bump(g_count_memalign_pass); return g_sys_posix_memalign(memptr, alignment, size); }
  bump(g_count_memalign_user);
  alloc8_record_alloc_size(size);
  *memptr = nullptr;
  if (alignment == 0 ||
      (alignment % sizeof(void*)) != 0 ||
      (alignment & (alignment - 1)) != 0) {
    return EINVAL;
  }
  void* ptr = xxmemalign(alignment, size);
  if (!ptr && size != 0) {
    return ENOMEM;
  }
  if (ptr) mark_owned(ptr, size ? size : 1);
  *memptr = ptr;
  return 0;
}

void* replace_valloc(size_t sz) {
  void* p = xxmemalign(ALLOC8_PAGE_SIZE, sz);
  if (p) mark_owned(p, sz ? sz : 1);
  return p;
}

void replace_vfree(void* ptr) { replace_free(ptr); }

// ─── FORK HANDLERS ────────────────────────────────────────────────────────────

void replace__malloc_fork_prepare() {
  xxmalloc_lock();
}

void replace__malloc_fork_parent() {
  xxmalloc_unlock();
}

void replace__malloc_fork_child() {
  xxmalloc_unlock();
}

// ─── PRINTF STUB ──────────────────────────────────────────────────────────────

void replace_malloc_printf(const char*, ...) {
  // NOP
}

} // extern "C"

// ─── MALLOC ZONE IMPLEMENTATION ───────────────────────────────────────────────
// Included from separate file for organization

#include "mac_zones.cpp"

// ─── INTERPOSITION TABLE ──────────────────────────────────────────────────────

// Core allocation functions. Every entry must go through the replace_*
// shim so foreign-pointer routing and ownership tracking stay in the loop —
// jumping straight to xxmalloc/xxfree is faster but bypasses both, which is
// fatal under modern libobjc/CoreFoundation that probe pointer ownership
// via malloc_size().
MAC_INTERPOSE(replace_malloc, malloc);
MAC_INTERPOSE(replace_free, free);
MAC_INTERPOSE(replace_calloc, calloc);
MAC_INTERPOSE(replace_realloc, realloc);
MAC_INTERPOSE(replace_reallocf, reallocf);
// Note: memalign doesn't exist on macOS, only posix_memalign
MAC_INTERPOSE(replace_aligned_alloc, aligned_alloc);
MAC_INTERPOSE(replace_posix_memalign, posix_memalign);
MAC_INTERPOSE(replace_valloc, valloc);
MAC_INTERPOSE(replace_vfree, vfree);
MAC_INTERPOSE(replace_strdup, strdup);
MAC_INTERPOSE(replace_malloc_usable_size, malloc_size);
MAC_INTERPOSE(replace_malloc_good_size, malloc_good_size);
MAC_INTERPOSE(replace_malloc_printf, malloc_printf);

// Fork handlers
MAC_INTERPOSE(replace__malloc_fork_prepare, _malloc_fork_prepare);
MAC_INTERPOSE(replace__malloc_fork_parent, _malloc_fork_parent);
MAC_INTERPOSE(replace__malloc_fork_child, _malloc_fork_child);

// C++ operators — must route through replace_* so ownership tracking and
// foreign-pointer detection stay correct. (Older alloc8 jumped to xxmalloc/
// xxfree directly here for inlining; the speed gain is not worth losing
// foreign-pointer safety on macOS.)
MAC_INTERPOSE(replace_malloc, _Znwm);
MAC_INTERPOSE(replace_malloc, _Znam);
MAC_INTERPOSE(replace_free,   _ZdlPv);
MAC_INTERPOSE(replace_free,   _ZdaPv);
MAC_INTERPOSE(replace_malloc, _ZnwmRKSt9nothrow_t);
MAC_INTERPOSE(replace_malloc, _ZnamRKSt9nothrow_t);
MAC_INTERPOSE(replace_free,   _ZdlPvRKSt9nothrow_t);
MAC_INTERPOSE(replace_free,   _ZdaPvRKSt9nothrow_t);

// Zone-API surface: only malloc_zone_from_ptr is interposed, and only as a
// hook point so we can recover libSystem's original implementation by
// scanning the __interpose section. Our replacement is transparent — it
// returns whatever libSystem itself would return — so callers like
// libsandbox's scheme parser (which compiles the sandbox profile during
// content-process startup), CoreFoundation, and libxpc keep seeing real
// libSystem zones and their bookkeeping stays consistent.
//
// We do NOT interpose malloc_default_zone, malloc_create_zone,
// malloc_get_all_zones, malloc_zone_register/unregister, or any of the
// malloc_zone_* operation entry points. Earlier alloc8 versions did, which
// substituted our zone for every default-zone reference and made libsandbox
// abort with
//   "BUG IN CLIENT OF LIBMALLOC: POINTER BEING FREED WAS NOT ALLOCATED"
// on the first sandbox compilation. Plain malloc/free/calloc/realloc and
// operator new/delete are still interposed via their symbol-level entries
// above, which covers the great majority of allocations.
MAC_INTERPOSE(replace_malloc_zone_from_ptr, malloc_zone_from_ptr);

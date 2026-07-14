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
  void  xxfree_sized(void*, size_t);
  void  xxfree_aligned_sized(void*, size_t, size_t);

  // ─── OPTIONAL OWNERSHIP HOOK (allocator-agnostic) ──────────────────────────
  //
  // An allocator MAY define xxowns(ptr): a predicate that returns true iff the
  // pointer was handed out by the allocator. It must be safe to call on any
  // address (including foreign libSystem/libobjc pointers) and have no false
  // positives or negatives.
  //
  // When present, alloc8 uses it as the ownership test on the free / realloc /
  // malloc_size hot paths instead of consulting its internal ptr->size table,
  // and skips populating that table on every allocation. This removes the
  // per-object hash-table traffic for allocators that can answer ownership
  // from their own metadata (e.g. an allocator whose memory comes from regions
  // it never unmaps). Allocators that do NOT define xxowns are unaffected:
  // g_have_owns stays false and the original size-table path runs.
  //
  // Weak default definitions. Allocators that can answer ownership from their
  // own metadata provide strong definitions that override these. The weak
  // definitions ensure the linker always resolves symbols (required for LTO).
  //
  // xxowns_active: true iff the allocator provides a real xxowns(). Alloc8
  // checks this once at init time to decide whether to skip the hash table.
  // xxowns: the ownership predicate itself.
  __attribute__((weak)) bool xxowns_active() { return false; }
  __attribute__((weak)) bool xxowns(const void*) { return false; }

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
  void  _ZdlPvm(void*, size_t);           // operator delete(void*, size_t)
  void  _ZdaPvm(void*, size_t);           // operator delete[](void*, size_t)

  // C++14+17 sized+aligned delete (need wrapper functions below)
  void  _ZdlPvmSt11align_val_t(void*, size_t, size_t);  // operator delete(void*, size_t, align_val_t)
  void  _ZdaPvmSt11align_val_t(void*, size_t, size_t);  // operator delete[](void*, size_t, align_val_t)
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
extern std::atomic<bool> g_fast;   // see mac_zones.cpp
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
  // Initialization finishes in a priority-101 load constructor, before any
  // user allocation, so on the hot path this is reliably already done.
  if (__builtin_expect(g_init_state.load(std::memory_order_acquire) != /*INIT_DONE=*/2, 0)) {
    alloc8_init_once();
  }
}
extern "C" void alloc8_dump_stats(int);
extern "C" void alloc8_record_alloc_size(size_t);

// True only when the user set ALLOC8_STATS. Off in normal use, so the
// counters/histogram are entirely skipped on the hot path (one predicted-not-
// taken branch instead of an atomic RMW + an out-of-line record call).
static inline bool stats_active() {
  return __builtin_expect(g_stats_enabled.load(std::memory_order_relaxed), 0);
}

static inline void bump(std::atomic<uint64_t>& c) {
  if (!stats_active()) return;
  uint64_t v = c.fetch_add(1, std::memory_order_relaxed) + 1;
  // Dump every N total user-allocator allocations from one chosen counter
  // (g_count_malloc_user is a good proxy for "is the user allocator active").
  // Cheap and signal-free; avoids both atexit/_exit issues and SIGUSR1
  // collisions inside Firefox.
  if (&c == &g_count_malloc_user && (v % 200000) == 0) alloc8_dump_stats(0);
}

// Inlinable wrapper around the (out-of-line) size-histogram recorder: the
// guard is inlined so the common stats-off path is a single branch with no
// call. Pairs the malloc-side counter bump with the histogram update so each
// allocation entry point does its stats work behind one check.
static inline void bump_and_record(std::atomic<uint64_t>& c, size_t sz) {
  if (!stats_active()) return;
  bump(c);
  alloc8_record_alloc_size(sz);
}

// ─── OWNERSHIP DISPATCH (xxowns hook vs. internal size table) ─────────────────
//
// Three dispatch modes, from fastest to slowest:
//
// 1. Compile-time inline hook (ALLOC8_XXOWNS_INLINE_HEADER): the embedding
//    allocator names a header that defines
//        static inline bool alloc8_xxowns_inline(const void* p);
//    The predicate must be safe on any address with no false positives or
//    negatives (same contract as xxowns()). Because the interpose sources
//    are compiled inside the consumer's target, the predicate inlines
//    straight into replace_free/replace_realloc/replace_malloc_size —
//    no call, no runtime "do we have a hook?" branch, and track/untrack
//    disappear entirely. The weak-symbol machinery below cannot achieve
//    this: ThinLTO will not inline a prevailing strong definition over a
//    module-local weak one, so xxowns() always costs a call per free.
//
// 2. Runtime xxowns() hook: allocator provides strong definitions of
//    xxowns()/xxowns_active(). g_have_owns is checked once at static-init
//    time and cached, so the hot path is a load + predicted branch + call.
//
// 3. Internal ptr->size table: no hook at all; every malloc/free pays a
//    hash insert/erase, and the table saturates at ~4M live objects.

#if defined(ALLOC8_XXOWNS_INLINE_HEADER)

// Escape the surrounding extern "C" block: the consumer's header may
// define templates or other C++-linkage-only constructs.
}
#include ALLOC8_XXOWNS_INLINE_HEADER
extern "C" {

static inline bool have_owns_hook() {
  return true;
}

static inline bool is_owned(const void* ptr) {
  return alloc8_xxowns_inline(ptr);
}

static inline void track_owned(const void*, size_t) {}
static inline void untrack_owned(const void*) {}

#else

// g_have_owns: true iff the allocator supplied xxowns(). Checked once at
// static-init time via xxowns_active() and cached as a plain global so the
// hot path is a single load + predicted branch.
static const bool g_have_owns = xxowns_active();

static inline bool have_owns_hook() {
  return g_have_owns;
}

// is_owned(): the ownership predicate used on every free / realloc /
// malloc_size. Prefers the allocator's xxowns() hook; otherwise falls back to
// the internal ptr->size table (maybe_owned). Both are safe on any address.
static inline bool is_owned(const void* ptr) {
  if (have_owns_hook()) return xxowns(ptr);
  return maybe_owned(ptr);
}

// track_owned() / untrack_owned(): maintain the internal size table. These are
// no-ops when xxowns() is present, since the table is then unused — that is
// the entire point of the hook (no per-object hash traffic). When absent they
// behave exactly as before.
static inline void track_owned(const void* ptr, size_t sz) {
  if (have_owns_hook()) return;
  mark_owned(ptr, sz);
}

static inline void untrack_owned(const void* ptr) {
  if (have_owns_hook()) return;
  forget_size(ptr);
}

#endif // ALLOC8_XXOWNS_INLINE_HEADER

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
//
// With the xxowns() hook the table is empty, so the size answer comes from
// xxmalloc_usable_size alone (the allocator must answer getSize for pointers
// it owns); the lookup_size fallback only applies in the non-hook path.
static inline size_t owned_size(void* ptr) {
  if (!is_owned(ptr)) return 0;
  size_t s = xxmalloc_usable_size(ptr);
  if (s) return s;
  if (have_owns_hook()) return 1;  // owned but unsized: best-effort non-zero.
  s = lookup_size(ptr);
  return s ? s : 1;
}

// Slow path: not yet initialized, or passthrough, or stats enabled.
static ALLOC8_NOINLINE void* replace_malloc_slow(size_t sz) {
  ensure_init();
  if (__builtin_expect(g_passthrough, 0)) { bump(g_count_malloc_pass); return g_sys_malloc(sz); }
  bump_and_record(g_count_malloc_user, sz);
  void* p = xxmalloc(sz);
  if (p) track_owned(p, sz ? sz : 1);
  return p;
}

void* replace_malloc(size_t sz) {
  ALLOC8_SET_CALLER_RA();
  // ONE acquire load gates init, passthrough and stats (see g_fast).
  if (__builtin_expect(g_fast.load(std::memory_order_acquire), 1)) {
    return xxmalloc(sz);
  }
  return replace_malloc_slow(sz);
}

static ALLOC8_NOINLINE void replace_free_slow(void* ptr);

void replace_free(void* ptr) {
  if (!ptr) return;
  // ONE acquire load gates init, passthrough and stats (see g_fast).
  if (__builtin_expect(g_fast.load(std::memory_order_acquire), 1)) {
    if (__builtin_expect(is_owned(ptr), 1)) {
      xxfree(ptr);
      return;
    }
    // Foreign pointer: fall through to the libSystem zone routing below.
  } else {
    replace_free_slow(ptr);
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

// Slow path for free: not yet initialized, or passthrough, or stats enabled.
static ALLOC8_NOINLINE void replace_free_slow(void* ptr) {
  ensure_init();
  if (__builtin_expect(g_passthrough, 0)) { bump(g_count_free_pass); g_sys_free(ptr); return; }
  if (__builtin_expect(is_owned(ptr), 1)) {
    bump(g_count_free_user);
    untrack_owned(ptr);
    xxfree(ptr);
    return;
  }
  if (malloc_zone_t* z = libsystem_zone_for(ptr)) {
    bump(g_count_free_libsys);
    z->free(z, ptr);
    return;
  }
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

// Shared realloc body. Does NOT store the caller-RA hint — the exported
// entries (replace_realloc, replace_reallocf) store it first, so the hint
// keeps the application's call site. Malloc fallbacks call xxmalloc directly
// instead of replace_malloc for the same reason (replace_malloc would
// overwrite the hint with an address inside this file).
static inline void* realloc_impl(void* ptr, size_t sz) {
  ensure_init();
  if (g_passthrough) { bump(g_count_realloc_pass); return g_sys_realloc(ptr, sz); }
  bump(g_count_realloc_user);
  alloc8_record_alloc_size(sz);
  if (!ptr) {
    void* p = xxmalloc(sz);
    if (p) mark_owned(p, sz ? sz : 1);
    return p;
  }

  // 0 size = free (macOS returns a small allocation)
  if (sz == 0) {
    replace_free(ptr);
    void* p = xxmalloc(1);
    if (p) mark_owned(p, 1);
    return p;
  }

  // Ours? Resize in our allocator.
  if (is_owned(ptr)) {
    size_t oldSize = owned_size(ptr);
    if ((oldSize / 2 < sz) && (sz <= oldSize)) return ptr;
    void* newPtr = xxmalloc(sz);
    if (newPtr) {
      track_owned(newPtr, sz);
      size_t copySize = (oldSize < sz) ? oldSize : sz;
      if (copySize) memcpy(newPtr, ptr, copySize);
      untrack_owned(ptr);
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
    track_owned(newPtr, sz);
    memcpy(newPtr, ptr, sz);  // best effort: may over-read; same risk libc has.
  }
  return newPtr;
}

void* replace_realloc(void* ptr, size_t sz) {
  ALLOC8_SET_CALLER_RA();
  return realloc_impl(ptr, sz);
}

// macOS-specific reallocf — same as realloc but always free original on failure.
void* replace_reallocf(void* ptr, size_t sz) {
  ALLOC8_SET_CALLER_RA();
  void* p = realloc_impl(ptr, sz);
  if (!p && ptr) replace_free(ptr);
  return p;
}

void* replace_calloc(size_t count, size_t size) {
  ALLOC8_SET_CALLER_RA();
  ensure_init();
  if (g_passthrough) { bump(g_count_calloc_pass); return g_sys_calloc(count, size); }
  bump_and_record(g_count_calloc_user, count * size);
  void* p = xxcalloc(count, size);
  if (p) track_owned(p, count * size);
  return p;
}

char* replace_strdup(const char* s) {
  ALLOC8_SET_CALLER_RA();
  if (!s) return nullptr;
  size_t len = strlen(s) + 1;
  char* newStr = (char*)xxmalloc(len);
  if (newStr) {
    track_owned(newStr, len);
    memcpy(newStr, s, len);
  }
  return newStr;
}

void* replace_memalign(size_t alignment, size_t size) {
  ALLOC8_SET_CALLER_RA();
  void* p = xxmemalign(alignment, size);
  if (p) track_owned(p, size ? size : 1);
  return p;
}

void* replace_aligned_alloc(size_t alignment, size_t size) {
  ALLOC8_SET_CALLER_RA();
  if (alignment == 0 || (size % alignment) != 0) {
    return nullptr;
  }
  void* p = xxmemalign(alignment, size);
  if (p) track_owned(p, size);
  return p;
}

int replace_posix_memalign(void** memptr, size_t alignment, size_t size) {
  ALLOC8_SET_CALLER_RA();
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
  if (ptr) track_owned(ptr, size ? size : 1);
  *memptr = ptr;
  return 0;
}

void* replace_valloc(size_t sz) {
  ALLOC8_SET_CALLER_RA();
  void* p = xxmemalign(ALLOC8_PAGE_SIZE, sz);
  if (p) track_owned(p, sz ? sz : 1);
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

// ─── C23 SIZED FREE / C++14 SIZED DELETE ─────────────────────────────────────
//
// Same foreign-pointer routing as replace_free, but pass the size hint through
// to xxfree_sized for pointers we own. Allocators that implement free_sized
// (e.g. DieHard's CombineHeap) get O(1) deallocation; others fall back to
// xxfree internally.

void replace_free_sized(void* ptr, size_t sz) {
  if (!ptr) return;
  ensure_init();
  if (g_passthrough) { bump(g_count_free_pass); g_sys_free(ptr); return; }
  if (is_owned(ptr)) {
    bump(g_count_free_user);
    untrack_owned(ptr);
    xxfree_sized(ptr, sz);
    return;
  }
  if (malloc_zone_t* z = libsystem_zone_for(ptr)) {
    bump(g_count_free_libsys);
    if (z->free_definite_size) z->free_definite_size(z, ptr, sz);
    else z->free(z, ptr);
    return;
  }
  bump(g_count_free_dropped);
}

void replace_free_aligned_sized(void* ptr, size_t alignment, size_t sz) {
  if (!ptr) return;
  ensure_init();
  if (g_passthrough) { bump(g_count_free_pass); g_sys_free(ptr); return; }
  if (is_owned(ptr)) {
    bump(g_count_free_user);
    untrack_owned(ptr);
    xxfree_aligned_sized(ptr, alignment, sz);
    return;
  }
  if (malloc_zone_t* z = libsystem_zone_for(ptr)) {
    bump(g_count_free_libsys);
    if (z->free_definite_size) z->free_definite_size(z, ptr, sz);
    else z->free(z, ptr);
    return;
  }
  bump(g_count_free_dropped);
}

// C++14+17 sized+aligned delete replacement (void*, size_t, align_val_t).
// Note: align_val_t is a scoped enum wrapping size_t, so ABI-compatible.
void replace_delete_sized_aligned(void* ptr, size_t sz, size_t alignment) {
  replace_free_aligned_sized(ptr, alignment, sz);
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
// C++14 sized delete (void*, size_t) and C++17 sized+aligned delete
// (void*, size_t, align_val_t). All variants go through replace_free_sized /
// replace_delete_sized_aligned, which do the same foreign-pointer routing as
// replace_free and pass the size hint through to xxfree_sized for pointers
// we own. C23 sized free interposers will be added once macOS libc exports
// the underlying symbols; for now `free_sized()` reaches us via the C++
// sized-delete operators and via malloc_zone_free_definite_size (mac_zones).
MAC_INTERPOSE(replace_free_sized, _ZdlPvm);
MAC_INTERPOSE(replace_free_sized, _ZdaPvm);
MAC_INTERPOSE(replace_delete_sized_aligned, _ZdlPvmSt11align_val_t);
MAC_INTERPOSE(replace_delete_sized_aligned, _ZdaPvmSt11align_val_t);

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

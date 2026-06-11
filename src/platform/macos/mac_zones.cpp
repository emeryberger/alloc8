// alloc8/src/platform/macos/mac_zones.cpp
// macOS malloc_zone_t implementation
//
// This file is included by mac_wrapper.cpp
// Reference: Heap-Layers macwrapper.cpp by Emery Berger

#ifndef __APPLE__
#error "This file is for macOS only"
#endif

#include <atomic>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

// ─── ALLOCATION OWNERSHIP TABLE ───────────────────────────────────────────────
//
// We keep a (ptr -> size) entry for every pointer the user allocator returns.
// Two jobs:
//   1. Ownership probe. xxmalloc_usable_size and xxfree are unsafe to call on
//      pointers the user allocator did not issue (DieHard / Heap-Layers /
//      Hoard happily dereference whatever we pass). A non-zero size lookup
//      proves the user allocator owns the pointer; anything else routes back
//      to libSystem.
//   2. Size fallback. Some allocators (DieHard's CombineHeap) return 0 from
//      getSize even on pointers they validly issued, which breaks libobjc's
//      `malloc_size(cls) >= sizeof(objc_class)` class-realization assert. The
//      stored size is the source of truth when xxmalloc_usable_size returns 0.
//
// Storage: sharded open-addressed table, one mmap per shard, lazily allocated.
// Sharding is the key concurrency guard — without it, every alloc/free across
// every thread fights over the same atomic cache lines. With kShardCount = 256
// the home shard is selected by the high bits of the splitmix hash, so
// uncontended allocator paths spread across all shards.

namespace {
constexpr size_t kShardCount      = 256;
constexpr size_t kShardBuckets    = 1u << 14;     // 16 K slots / shard → 4 M total
constexpr size_t kShardMask       = kShardBuckets - 1;
constexpr size_t kShardProbeLimit = 64;

struct SizeEntry {
  std::atomic<uintptr_t> key;     // 0 = empty, kTombstone = deleted
  std::atomic<size_t>    value;
};
constexpr uintptr_t kTombstone = ~uintptr_t(0);

struct Shard {
  std::atomic<SizeEntry*> table{nullptr};
};

static Shard g_shards[kShardCount];

inline uint64_t splitmix64(uintptr_t p) {
  uint64_t x = static_cast<uint64_t>(p);
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

inline size_t shard_for(uint64_t h) { return (h >> 32) % kShardCount; }
inline size_t bucket_for(uint64_t h) { return h & kShardMask; }

static SizeEntry* ensure_shard_table(Shard& sh) {
  SizeEntry* t = sh.table.load(std::memory_order_acquire);
  if (t) return t;
  size_t bytes = kShardBuckets * sizeof(SizeEntry);
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  SizeEntry* nt = reinterpret_cast<SizeEntry*>(p);
  SizeEntry* expected = nullptr;
  if (!sh.table.compare_exchange_strong(expected, nt,
                                        std::memory_order_acq_rel)) {
    munmap(p, bytes);
    return expected;
  }
  return nt;
}
}  // namespace

static void record_size(const void* ptr, size_t sz) {
  if (!ptr || !sz) return;
  uint64_t h = splitmix64(reinterpret_cast<uintptr_t>(ptr));
  Shard& sh = g_shards[shard_for(h)];
  SizeEntry* t = ensure_shard_table(sh);
  if (!t) return;
  uintptr_t key = reinterpret_cast<uintptr_t>(ptr);
  size_t home = bucket_for(h);
  for (size_t i = 0; i < kShardProbeLimit; ++i) {
    SizeEntry& e = t[(home + i) & kShardMask];
    uintptr_t k = e.key.load(std::memory_order_acquire);
    if (k == 0 || k == kTombstone || k == key) {
      uintptr_t expect = k;
      if (e.key.compare_exchange_strong(expect, key,
                                        std::memory_order_acq_rel)) {
        e.value.store(sz, std::memory_order_release);
        return;
      }
      --i;  // someone else took the slot; retry the same index
    }
  }
  // Shard saturated within probe window. Don't overwrite — losing an entry
  // means a future malloc_size returns 0 and libobjc aborts. We choose to
  // accept the (rare) untracked allocation; the same pointer will get a
  // best-effort `1` from owned_size() once we can't find it.
}

static size_t lookup_size(const void* ptr) {
  if (!ptr) return 0;
  uint64_t h = splitmix64(reinterpret_cast<uintptr_t>(ptr));
  Shard& sh = g_shards[shard_for(h)];
  SizeEntry* t = sh.table.load(std::memory_order_acquire);
  if (!t) return 0;
  uintptr_t key = reinterpret_cast<uintptr_t>(ptr);
  size_t home = bucket_for(h);
  for (size_t i = 0; i < kShardProbeLimit; ++i) {
    SizeEntry& e = t[(home + i) & kShardMask];
    uintptr_t k = e.key.load(std::memory_order_acquire);
    if (k == 0) return 0;
    if (k == key) return e.value.load(std::memory_order_acquire);
  }
  return 0;
}

static void forget_size(const void* ptr) {
  if (!ptr) return;
  uint64_t h = splitmix64(reinterpret_cast<uintptr_t>(ptr));
  Shard& sh = g_shards[shard_for(h)];
  SizeEntry* t = sh.table.load(std::memory_order_acquire);
  if (!t) return;
  uintptr_t key = reinterpret_cast<uintptr_t>(ptr);
  size_t home = bucket_for(h);
  for (size_t i = 0; i < kShardProbeLimit; ++i) {
    SizeEntry& e = t[(home + i) & kShardMask];
    uintptr_t k = e.key.load(std::memory_order_acquire);
    if (k == 0) return;
    if (k == key) {
      e.key.store(kTombstone, std::memory_order_release);
      return;
    }
  }
}

// Ownership probe = "we have a non-zero size record for this pointer".
// No coarser bitmap layer because that produced false positives whenever
// libsystem and the user allocator shared a memory region — DieHard's
// internal STL containers got fed libsystem pointers and corrupted state.
static inline bool maybe_owned(const void* ptr) {
  return lookup_size(ptr) != 0;
}

static inline void mark_owned(const void* ptr, size_t sz) {
  record_size(ptr, sz);
}

// ─── DEFAULT ZONE ─────────────────────────────────────────────────────────────

static const char* theOneTrueZoneName = "alloc8DefaultZone";

static bool initializeZone(malloc_zone_t& zone);

static malloc_zone_t* getDefaultZone() {
  static malloc_zone_t theDefaultZone;
  static bool initialized = initializeZone(theDefaultZone);
  (void)initialized;
  return &theDefaultZone;
}

// ─── LIBSYSTEM ZONE CAPTURE (foreign-pointer routing) ─────────────────────────
//
// Modern libobjc / CoreFoundation validate heap pointers via malloc_size() and
// expect a non-zero size for any pointer they hold. Many of those pointers are
// allocated through libSystem-internal paths that DYLD interposition cannot
// see (intra-libSystem calls bypass __DATA,__interpose). When such a pointer
// later flows back through an interposed entry point (malloc_size, free,
// realloc, malloc_zone_from_ptr), we must NOT hand it to the user allocator —
// the user allocator typically cannot recognize foreign pointers and will
// either return 0 (DieHard: triggers libobjc's "corrupt data pointer" abort)
// or dereference garbage (Hoard: PAC-auth SIGSEGV).
//
// To route foreign pointers correctly we keep a captured pointer to libSystem's
// real default zone and ask it first. Each malloc_zone_t carries a `size`
// callback that returns 0 for pointers it does not own — this is the canonical
// safe ownership probe.

// Lazy because we may be called from an interposer before our constructor
// has run (e.g., during a system framework's own dyld initializer chain).
// std::atomic-on-pointer is the entire synchronization budget — capture is
// idempotent and the only racy scenario is two threads doing the same scan.
#include <atomic>

using ZoneFromPtrFn = malloc_zone_t* (*)(const void*);

static ZoneFromPtrFn resolve_libsystem_zone_from_ptr();

// Sentinel meaning "we already tried to resolve and it's unavailable."
static ZoneFromPtrFn const kZoneFromPtrUnavailable =
    reinterpret_cast<ZoneFromPtrFn>(uintptr_t{1});

static std::atomic<ZoneFromPtrFn> g_libsystem_zone_from_ptr{nullptr};

// Returns the libSystem zone that owns ptr (default, helper, scalable,
// purgeable, …) or nullptr if no libSystem zone owns it. Probes via
// libSystem's own malloc_zone_from_ptr, which walks every registered libSystem
// zone and asks each one's size callback. We never register our own zone, so a
// non-null return is guaranteed to be a libSystem zone — exactly the routing
// signal we need to keep foreign pointers out of the user allocator.
static inline malloc_zone_t* libsystem_zone_for(const void* ptr) {
  if (!ptr) return nullptr;
  ZoneFromPtrFn fn = g_libsystem_zone_from_ptr.load(std::memory_order_acquire);
  if (!fn) {
    fn = resolve_libsystem_zone_from_ptr();
    g_libsystem_zone_from_ptr.store(fn ? fn : kZoneFromPtrUnavailable,
                                    std::memory_order_release);
    if (!fn) fn = kZoneFromPtrUnavailable;
  }
  if (fn == kZoneFromPtrUnavailable) return nullptr;
  return fn(ptr);
}

// Resolve libSystem's default malloc zone, bypassing our own interposers.
//
// dyld processes __DATA,__interpose entries before running our constructors,
// so by the time we run, calls to malloc_default_zone() are already routed
// to replace_malloc_default_zone() — including symbol lookups via
// dlsym(RTLD_DEFAULT/handle, "malloc_default_zone"), which return our wrapper.
//
// To recover the real libsystem function pointer, we scan our own image's
// __interpose section. Each entry is a (replacement, original) pair where
// `original` was bound by the static linker to libsystem's actual
// malloc_default_zone — that pointer is *not* rewritten by dyld at load time.
// On ARM64e the section lives in __AUTH_CONST instead of __DATA.
static void* find_interpose_original(void* replacement) {
  // Modern toolchains place __interpose in __DATA_CONST; older ones use __DATA;
  // ARM64e uses __AUTH_CONST. Check all three.
  static const char* segments[] = {"__DATA_CONST", "__AUTH_CONST", "__DATA"};
  uint32_t n = _dyld_image_count();
  for (uint32_t i = 0; i < n; i++) {
    auto* hdr = reinterpret_cast<const struct mach_header_64*>(
        _dyld_get_image_header(i));
    if (!hdr) continue;
    for (auto* seg : segments) {
      unsigned long sz = 0;
      auto* data = getsectiondata(hdr, seg, "__interpose", &sz);
      if (!data || sz == 0) continue;
      size_t count = sz / (2 * sizeof(void*));
      auto* entries = reinterpret_cast<void* const*>(data);
      for (size_t j = 0; j < count; j++) {
        if (entries[j * 2] == replacement) {
          return entries[j * 2 + 1];
        }
      }
    }
  }
  return nullptr;
}

extern "C" malloc_zone_t* replace_malloc_zone_from_ptr(const void*);

static ZoneFromPtrFn resolve_libsystem_zone_from_ptr() {
  auto fn = reinterpret_cast<ZoneFromPtrFn>(
      find_interpose_original(reinterpret_cast<void*>(&replace_malloc_zone_from_ptr)));
  if (getenv("ALLOC8_DEBUG")) {
    fprintf(stderr, "[alloc8] resolve_libsystem_zone_from_ptr: replace=%p orig=%p\n",
            (void*)&replace_malloc_zone_from_ptr, (void*)fn);
  }
  return fn;
}

// ─── PASSTHROUGH MODE ─────────────────────────────────────────────────────────
//
// Firefox child processes (plugin-container, Nightly GPU Helper, ...) run
// libsandbox's scheme parser during sandbox profile compilation. That parser
// uses its own libSystem malloc zones; if our interposers (or any user
// allocator we route through) get involved, libsandbox aborts with
// "BUG IN CLIENT OF LIBMALLOC: POINTER BEING FREED WAS NOT ALLOCATED".
//
// We therefore detect Firefox child processes at constructor time and switch
// alloc8 into "passthrough" mode: every replace_* entry point becomes a thin
// trampoline to libSystem's real implementation, captured via the same
// __interpose-section scan we use for malloc_zone_from_ptr. The user
// allocator (Hoard / DieHard / smash / ...) is never called in those
// processes; the parent firefox process keeps using it as before.
//
// Override with ALLOC8_PASSTHROUGH=0 (force off) or ALLOC8_PASSTHROUGH=1
// (force on) for debugging.

extern "C" {
  void* replace_malloc(size_t);
  void  replace_free(void*);
  void* replace_calloc(size_t, size_t);
  void* replace_realloc(void*, size_t);
  int   replace_posix_memalign(void**, size_t, size_t);
  size_t replace_malloc_usable_size(void*);
}

bool   g_passthrough = false;
void* (*g_sys_malloc)(size_t)                      = nullptr;
void  (*g_sys_free)(void*)                         = nullptr;
void* (*g_sys_calloc)(size_t, size_t)              = nullptr;
void* (*g_sys_realloc)(void*, size_t)              = nullptr;
int   (*g_sys_posix_memalign)(void**, size_t, size_t) = nullptr;
size_t(*g_sys_malloc_size)(const void*)            = nullptr;

// Detected once, on the first interposed allocator call. We can't rely on a
// constructor for this — system constructors at the default priority start
// allocating before our priority-101 init runs, so we'd already have routed
// some allocations through the user allocator by the time we know we should
// be in passthrough. Once `g_init_state` is INIT_DONE, the values above are
// stable.
namespace {
constexpr int INIT_PENDING  = 0;
constexpr int INIT_RUNNING  = 1;
constexpr int INIT_DONE     = 2;
}
std::atomic<int> g_init_state{INIT_PENDING};

// Counters. Bumped from every replace_* entry point so we can prove the user
// allocator is being exercised (vs. silently passthrough'd to libSystem).
// Enabled only when ALLOC8_STATS is set in the environment, to avoid the
// per-call atomic write in normal use.
std::atomic<bool>     g_stats_enabled{false};
std::atomic<uint64_t> g_count_malloc_user{0};
std::atomic<uint64_t> g_count_malloc_pass{0};
std::atomic<uint64_t> g_count_calloc_user{0};
std::atomic<uint64_t> g_count_calloc_pass{0};
std::atomic<uint64_t> g_count_realloc_user{0};
std::atomic<uint64_t> g_count_realloc_pass{0};
std::atomic<uint64_t> g_count_memalign_user{0};
std::atomic<uint64_t> g_count_memalign_pass{0};
std::atomic<uint64_t> g_count_free_user{0};
std::atomic<uint64_t> g_count_free_pass{0};
std::atomic<uint64_t> g_count_free_libsys{0};
std::atomic<uint64_t> g_count_free_dropped{0};
std::atomic<uint64_t> g_count_size_user{0};
std::atomic<uint64_t> g_count_size_libsys{0};
std::atomic<uint64_t> g_count_size_pass{0};

// Allocation-size histogram. Bucket b counts requests where the requested
// size falls in [2^b, 2^(b+1)). Bucket 0 includes zero-size requests. 32
// buckets cover 1 byte through 4 GB, plenty for any realistic allocator.
constexpr int kHistBuckets = 32;
std::atomic<uint64_t> g_size_hist[kHistBuckets]{};

static bool detect_passthrough() {
  if (const char* env = getenv("ALLOC8_PASSTHROUGH")) {
    if (env[0] == '0') return false;
    if (env[0] == '1') return true;
  }
  // Auto-detect Firefox child processes. The parent is `firefox`; children
  // are `plugin-container` or "Nightly XXX Helper" launched via the .app
  // helper bundles.
  const char* prog = getprogname();
  if (!prog) return false;
  static const char* const kChildren[] = {
    "plugin-container",
    "Nightly GPU Helper",
    "Nightly Media Plugin Helper",
    "Nightly Security Module Helper",
  };
  for (auto* p : kChildren) {
    size_t n = 0; while (p[n]) ++n;
    size_t m = 0; while (prog[m]) ++m;
    if (m >= n) {
      bool eq = true;
      for (size_t i = 0; i < n; ++i) if (p[i] != prog[i]) { eq = false; break; }
      if (eq) return true;
    }
  }
  return false;
}

static void capture_system_allocs() {
  using MFn = void* (*)(size_t);
  using FFn = void  (*)(void*);
  using CFn = void* (*)(size_t, size_t);
  using RFn = void* (*)(void*, size_t);
  using PFn = int   (*)(void**, size_t, size_t);
  using SFn = size_t(*)(const void*);
  g_sys_malloc  = reinterpret_cast<MFn>(find_interpose_original(reinterpret_cast<void*>(&replace_malloc)));
  g_sys_free    = reinterpret_cast<FFn>(find_interpose_original(reinterpret_cast<void*>(&replace_free)));
  g_sys_calloc  = reinterpret_cast<CFn>(find_interpose_original(reinterpret_cast<void*>(&replace_calloc)));
  g_sys_realloc = reinterpret_cast<RFn>(find_interpose_original(reinterpret_cast<void*>(&replace_realloc)));
  g_sys_posix_memalign = reinterpret_cast<PFn>(find_interpose_original(reinterpret_cast<void*>(&replace_posix_memalign)));
  g_sys_malloc_size    = reinterpret_cast<SFn>(find_interpose_original(reinterpret_cast<void*>(&replace_malloc_usable_size)));
}

// Force zone initialization very early during library load.
// Priority 101 runs after basic C++ runtime setup (priority ~100) but before
// most other constructors. This ensures the zone is ready before dyld triggers
// any interposed malloc calls.
extern "C" void alloc8_dump_stats(int);

// Initialize the libSystem-zone capture, the system-malloc function table,
// and the passthrough flag. Idempotent — the first caller wins; later callers
// spin until INIT_DONE so they observe the captured pointers.
static void alloc8_init_once() {
  int expected = INIT_PENDING;
  if (g_init_state.compare_exchange_strong(expected, INIT_RUNNING,
                                           std::memory_order_acq_rel)) {
    (void)getDefaultZone();
    if (auto fn = resolve_libsystem_zone_from_ptr()) {
      g_libsystem_zone_from_ptr.store(fn, std::memory_order_release);
    }
    capture_system_allocs();
    g_passthrough = detect_passthrough();
    g_stats_enabled.store(getenv("ALLOC8_STATS") != nullptr,
                          std::memory_order_release);
    if (g_passthrough && getenv("ALLOC8_DEBUG")) {
      fprintf(stderr, "[alloc8] passthrough enabled for proc=%s\n",
              getprogname() ? getprogname() : "?");
    }
    g_init_state.store(INIT_DONE, std::memory_order_release);
  } else {
    while (g_init_state.load(std::memory_order_acquire) != INIT_DONE) {
      // Spin: another thread is initializing. Allocator interposers may be
      // called concurrently from multiple dyld constructors.
    }
  }
}

// Strip DYLD_INSERT_LIBRARIES from the environment in the parent process so
// that children inherit it cleanly. Without this, every child (plugin-
// container, GPU helper, ...) loads our dylib too. In children, libxpc /
// libsandbox / libsystem run in the very-early dyld-init phase using their
// own private malloc zones — interposing free() in that window aborts with
// "BUG IN CLIENT OF LIBMALLOC: POINTER BEING FREED WAS NOT ALLOCATED".
//
// Injecting only into the parent process is the standard pattern for macOS
// allocator-replacement testing (the parent is where the bulk of the work
// happens — chrome/JS thread, layout, networking; content processes do their
// own allocator work that's separately interesting but not what the user is
// testing here). Override with ALLOC8_NO_STRIP=1 if you do want injection in
// children.
__attribute__((constructor(102)))
static void alloc8_strip_dyld_for_children() {
  if (getenv("ALLOC8_NO_STRIP")) return;
  if (detect_passthrough()) return;  // we ARE a child; nothing to do
  unsetenv("DYLD_INSERT_LIBRARIES");
}

__attribute__((constructor(101)))
static void alloc8_early_zone_init() { alloc8_init_once(); }

// Bump the histogram bucket for a size argument. Called on every malloc /
// calloc / realloc / posix_memalign that lands in the user allocator (we
// don't care about the size of pointers we passthrough or free, since those
// don't tell us anything about the user allocator's load).
extern "C" void alloc8_record_alloc_size(size_t sz) {
  if (!g_stats_enabled.load(std::memory_order_relaxed)) return;
  int b = (sz <= 1) ? 0 : (63 - __builtin_clzll(sz));
  if (b < 0) b = 0;
  if (b >= kHistBuckets) b = kHistBuckets - 1;
  g_size_hist[b].fetch_add(1, std::memory_order_relaxed);
}

extern "C" void alloc8_dump_stats(int /*sig*/) {
  if (!g_stats_enabled.load(std::memory_order_acquire)) return;
  auto load = [](std::atomic<uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
  };
  uint64_t mu = load(g_count_malloc_user),    mp = load(g_count_malloc_pass);
  uint64_t cu = load(g_count_calloc_user),    cp = load(g_count_calloc_pass);
  uint64_t ru = load(g_count_realloc_user),   rp = load(g_count_realloc_pass);
  uint64_t au = load(g_count_memalign_user),  ap = load(g_count_memalign_pass);
  uint64_t fu = load(g_count_free_user),      fp = load(g_count_free_pass);
  uint64_t fl = load(g_count_free_libsys),    fd = load(g_count_free_dropped);
  uint64_t su = load(g_count_size_user);
  uint64_t sl = load(g_count_size_libsys),    sp = load(g_count_size_pass);
  uint64_t total_user = mu + cu + ru + au + fu + su;
  uint64_t total_pass = mp + cp + rp + ap + fp + sp;
  fprintf(stderr,
    "\n=== alloc8 stats (proc=%s, pid=%d, passthrough=%d) ===\n"
    "  malloc:           %12llu user  %12llu passthrough\n"
    "  calloc:           %12llu user  %12llu passthrough\n"
    "  realloc:          %12llu user  %12llu passthrough\n"
    "  posix_memalign:   %12llu user  %12llu passthrough\n"
    "  free:             %12llu user  %12llu passthrough\n"
    "  free → libSystem: %12llu  free dropped (foreign): %llu\n"
    "  malloc_size:      %12llu user  %12llu libSystem  %12llu passthrough\n"
    "  --------------------------------------------------\n"
    "  total user-allocator calls:     %llu\n"
    "  total passthrough calls:        %llu\n",
    getprogname() ? getprogname() : "?", (int)getpid(), (int)g_passthrough,
    (unsigned long long)mu, (unsigned long long)mp,
    (unsigned long long)cu, (unsigned long long)cp,
    (unsigned long long)ru, (unsigned long long)rp,
    (unsigned long long)au, (unsigned long long)ap,
    (unsigned long long)fu, (unsigned long long)fp,
    (unsigned long long)fl, (unsigned long long)fd,
    (unsigned long long)su, (unsigned long long)sl, (unsigned long long)sp,
    (unsigned long long)total_user, (unsigned long long)total_pass);

  // Histogram of requested sizes (user-allocator path only). Skip leading
  // and trailing empty buckets to keep the table compact.
  uint64_t hist[kHistBuckets];
  uint64_t hist_total = 0;
  int first = -1, last = -1;
  for (int i = 0; i < kHistBuckets; ++i) {
    hist[i] = g_size_hist[i].load(std::memory_order_relaxed);
    hist_total += hist[i];
    if (hist[i]) {
      if (first < 0) first = i;
      last = i;
    }
  }
  if (hist_total > 0) {
    fprintf(stderr,
      "  --------------------------------------------------\n"
      "  alloc-size histogram (request size at malloc/calloc/realloc/memalign):\n");
    for (int i = first; i <= last; ++i) {
      if (hist[i] == 0) continue;
      // Bucket i spans [2^i, 2^(i+1)).  Bucket 0 is "≤1 byte".
      char range[64];
      if (i == 0) {
        snprintf(range, sizeof(range), "          ≤ 1 B");
      } else if (i < 10) {
        snprintf(range, sizeof(range), "  %4llu B – %4llu B",
                 (unsigned long long)(1ULL << i),
                 (unsigned long long)((1ULL << (i + 1)) - 1));
      } else if (i < 20) {
        snprintf(range, sizeof(range), " %4llu KB – %4llu KB",
                 (unsigned long long)(1ULL << (i - 10)),
                 (unsigned long long)((1ULL << (i + 1 - 10)) - 1));
      } else if (i < 30) {
        snprintf(range, sizeof(range), " %4llu MB – %4llu MB",
                 (unsigned long long)(1ULL << (i - 20)),
                 (unsigned long long)((1ULL << (i + 1 - 20)) - 1));
      } else {
        snprintf(range, sizeof(range), " %4llu GB+",
                 (unsigned long long)(1ULL << (i - 30)));
      }
      double pct = 100.0 * (double)hist[i] / (double)hist_total;
      // Crude bar chart: one '#' per ~2.5%.
      char bar[42] = {0};
      int bars = (int)(pct / 2.5);
      if (bars > 40) bars = 40;
      for (int j = 0; j < bars; ++j) bar[j] = '#';
      fprintf(stderr, "  %s  %12llu  %5.1f%%  %s\n",
              range, (unsigned long long)hist[i], pct, bar);
    }
    fprintf(stderr, "  total sized requests: %llu\n",
            (unsigned long long)hist_total);
  }
  fprintf(stderr, "===================================================\n");
}

// ─── ZONE FUNCTION IMPLEMENTATIONS ────────────────────────────────────────────

extern "C" {

size_t replace_internal_malloc_zone_size(malloc_zone_t*, const void* ptr) {
  // This is OUR zone's size callback. Return non-zero ONLY if the pointer is
  // owned by the user allocator and we actually issued it (the bitmap proves
  // it). If libSystem owns it, return 0 so libSystem (not we) gets to satisfy
  // the query. If the bitmap doesn't recognize it, also return 0 — calling
  // xxmalloc_usable_size() on a non-issued pointer is undefined behavior.
  if (!ptr) return 0;
  if (libsystem_zone_for(ptr)) return 0;
  if (!is_owned(ptr)) return 0;
  size_t s = xxmalloc_usable_size((void*)ptr);
  if (s) return s;
  if (have_owns_hook()) return 1;  // owned but unsized; table is unused.
  s = lookup_size(ptr);
  return s ? s : 1;
}

malloc_zone_t* replace_malloc_create_zone(vm_size_t, unsigned) {
  return getDefaultZone();
}

malloc_zone_t* replace_malloc_default_zone() {
  return getDefaultZone();
}

malloc_zone_t* replace_malloc_default_purgeable_zone() {
  return getDefaultZone();
}

void replace_malloc_destroy_zone(malloc_zone_t*) {
  // NOP - we don't actually destroy zones
}

kern_return_t replace_malloc_get_all_zones(
    task_t,
    memory_reader_t,
    vm_address_t** addresses,
    unsigned* count) {
  *addresses = nullptr;
  *count = 0;
  return KERN_SUCCESS;
}

const char* replace_malloc_get_zone_name(malloc_zone_t* zone) {
  return zone->zone_name;
}

void replace_malloc_set_zone_name(malloc_zone_t*, const char*) {
  // NOP
}

int replace_malloc_jumpstart(int) {
  return 1;
}

// ─── ZONE ALLOCATION FUNCTIONS ────────────────────────────────────────────────

void* replace_malloc_zone_malloc(malloc_zone_t*, size_t size) {
  return replace_malloc(size);
}

void* replace_malloc_zone_calloc(malloc_zone_t*, size_t count, size_t size) {
  return replace_calloc(count, size);
}

void* replace_malloc_zone_realloc(malloc_zone_t*, void* ptr, size_t size) {
  return replace_realloc(ptr, size);
}

void* replace_malloc_zone_valloc(malloc_zone_t*, size_t size) {
  return replace_valloc(size);
}

void* replace_malloc_zone_memalign(malloc_zone_t*, size_t alignment, size_t size) {
  return replace_memalign(alignment, size);
}

void replace_malloc_zone_free(malloc_zone_t*, void* ptr) {
  if (!ptr) return;
  if (malloc_zone_t* z = libsystem_zone_for(ptr)) {
    z->free(z, ptr);
    return;
  }
  xxfree(ptr);
}

void replace_malloc_zone_free_definite_size(malloc_zone_t*, void* ptr, size_t sz) {
  if (!ptr) return;
  // Foreign pointers go straight to their libSystem zone — we never owned them.
  if (malloc_zone_t* z = libsystem_zone_for(ptr)) {
    if (z->free_definite_size) z->free_definite_size(z, ptr, sz);
    else z->free(z, ptr);
    return;
  }
  // Ours: pass the size hint through so allocators that implement free_sized
  // can use the O(1) path; xxfree_sized falls back to xxfree internally.
  untrack_owned(ptr);
  xxfree_sized(ptr, sz);
}

// ─── ZONE BATCH OPERATIONS ────────────────────────────────────────────────────

unsigned replace_malloc_zone_batch_malloc(
    malloc_zone_t*,
    size_t size,
    void** results,
    unsigned num_requested) {
  for (unsigned i = 0; i < num_requested; i++) {
    results[i] = replace_malloc(size);
    if (!results[i]) {
      return i;
    }
  }
  return num_requested;
}

void replace_malloc_zone_batch_free(
    malloc_zone_t*,
    void** to_be_freed,
    unsigned num) {
  for (unsigned i = 0; i < num; i++) {
    xxfree(to_be_freed[i]);
  }
}

// ─── ZONE INTROSPECTION ───────────────────────────────────────────────────────

bool replace_malloc_zone_check(malloc_zone_t*) {
  return true;
}

// Transparent passthrough: this exists only so the __interpose section has an
// entry pointing at libSystem's real malloc_zone_from_ptr (which we recover
// in libsystem_zone_for via the __interpose-section scan). Callers including
// libsandbox depend on the returned zone being a *real* libSystem zone, not
// our fiction — claiming our default zone for owned pointers caused
// libsystem free to abort during plugin-container sandbox compilation.
malloc_zone_t* replace_malloc_zone_from_ptr(const void* ptr) {
  return libsystem_zone_for(ptr);
}

void replace_malloc_zone_log(malloc_zone_t*, void*) {
  // NOP
}

void replace_malloc_zone_print(malloc_zone_t*, bool) {
  // NOP
}

void replace_malloc_zone_print_ptr_info(void*) {
  // NOP
}

void replace_malloc_zone_register(malloc_zone_t*) {
  // NOP
}

void replace_malloc_zone_unregister(malloc_zone_t*) {
  // NOP
}

} // extern "C"

// ─── ZONE INITIALIZATION ──────────────────────────────────────────────────────

static bool initializeZone(malloc_zone_t& zone) {
  zone.size = replace_internal_malloc_zone_size;
  zone.malloc = replace_malloc_zone_malloc;
  zone.calloc = replace_malloc_zone_calloc;
  zone.valloc = replace_malloc_zone_valloc;
  zone.free = replace_malloc_zone_free;
  zone.realloc = replace_malloc_zone_realloc;
  zone.destroy = replace_malloc_destroy_zone;
  zone.zone_name = theOneTrueZoneName;
  zone.batch_malloc = replace_malloc_zone_batch_malloc;
  zone.batch_free = replace_malloc_zone_batch_free;
  zone.introspect = nullptr;
  zone.version = 8;
  zone.memalign = replace_malloc_zone_memalign;

#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1060
  zone.free_definite_size = replace_malloc_zone_free_definite_size;
  zone.pressure_relief = nullptr;
#endif

  return true;
}

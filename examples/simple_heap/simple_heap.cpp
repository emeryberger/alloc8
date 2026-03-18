// alloc8/examples/simple_heap/simple_heap.cpp
// Example: A simple malloc wrapper that tracks allocation statistics
//
// This demonstrates how to use alloc8 to create a custom allocator.

#include <alloc8/alloc8.h>

#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#include <dlfcn.h>
#endif

// ─── REAL LIBC FUNCTIONS ─────────────────────────────────────────────────────
// When interposed, we need to call the real libc functions, not our wrappers

#if defined(__linux__)
namespace {

using malloc_fn = void* (*)(size_t);
using free_fn = void (*)(void*);
using aligned_alloc_fn = void* (*)(size_t, size_t);
using malloc_usable_size_fn = size_t (*)(void*);

// Init buffer for early allocations before dlsym completes
static constexpr size_t INIT_BUFFER_SIZE = 65536;
static char g_initBuffer[INIT_BUFFER_SIZE];
static size_t g_initBufferPos = 0;
static bool g_initializing = false;

malloc_fn real_malloc = nullptr;
free_fn real_free = nullptr;
aligned_alloc_fn real_aligned_alloc = nullptr;
malloc_usable_size_fn real_malloc_usable_size = nullptr;

// Check if pointer is from init buffer
inline bool is_init_buffer_ptr(void* ptr) {
  auto p = reinterpret_cast<char*>(ptr);
  return p >= g_initBuffer && p < g_initBuffer + INIT_BUFFER_SIZE;
}

// Allocate from init buffer
void* init_buffer_alloc(size_t sz) {
  // Align to 16 bytes
  size_t aligned_pos = (g_initBufferPos + 15) & ~15;
  if (aligned_pos + sz > INIT_BUFFER_SIZE) {
    return nullptr;
  }
  void* ptr = g_initBuffer + aligned_pos;
  g_initBufferPos = aligned_pos + sz;
  return ptr;
}

// Lazy initialization with recursion guard
void ensure_real_functions() {
  if (real_malloc || g_initializing) return;
  g_initializing = true;
  real_malloc = (malloc_fn)dlsym(RTLD_NEXT, "malloc");
  real_free = (free_fn)dlsym(RTLD_NEXT, "free");
  real_aligned_alloc = (aligned_alloc_fn)dlsym(RTLD_NEXT, "aligned_alloc");
  real_malloc_usable_size = (malloc_usable_size_fn)dlsym(RTLD_NEXT, "malloc_usable_size");
  g_initializing = false;
}

} // namespace
#endif

// ─── STATISTICS ───────────────────────────────────────────────────────────────

static std::atomic<size_t> g_totalAllocated{0};
static std::atomic<size_t> g_totalFreed{0};
static std::atomic<size_t> g_allocCount{0};
static std::atomic<size_t> g_freeCount{0};
static std::atomic<size_t> g_peakUsage{0};

// ─── SIMPLE HEAP IMPLEMENTATION ───────────────────────────────────────────────

/**
 * SimpleHeap: Wraps system malloc with statistics tracking.
 *
 * This is a minimal example to demonstrate the alloc8 API.
 * In practice, you'd implement a real allocator here.
 */
class SimpleHeap {
  std::mutex mutex_;

public:
  void* malloc(size_t sz) {
    // Use real system malloc as backing allocator (avoid recursion under LD_PRELOAD)
#if defined(__linux__)
    ensure_real_functions();
    void* ptr;
    if (g_initializing) {
      // During dlsym, use init buffer
      ptr = init_buffer_alloc(sz);
    } else if (real_malloc) {
      ptr = real_malloc(sz);
    } else {
      ptr = std::malloc(sz);
    }
#else
    void* ptr = std::malloc(sz);
#endif

    if (ptr) {
      g_totalAllocated += sz;
      g_allocCount++;

      // Update peak
      size_t current = g_totalAllocated - g_totalFreed;
      size_t peak = g_peakUsage.load();
      while (current > peak && !g_peakUsage.compare_exchange_weak(peak, current)) {
        // retry
      }
    }

    return ptr;
  }

  void free(void* ptr) {
    if (!ptr) return;

#if defined(__linux__)
    // Don't free init buffer allocations
    if (is_init_buffer_ptr(ptr)) return;
#endif

    size_t sz = getSize(ptr);
    g_totalFreed += sz;
    g_freeCount++;

#if defined(__linux__)
    if (real_free) real_free(ptr); else std::free(ptr);
#else
    std::free(ptr);
#endif
  }

  void* memalign(size_t alignment, size_t sz) {
    void* ptr = nullptr;

#if defined(_WIN32)
    ptr = _aligned_malloc(sz, alignment);
#elif defined(__APPLE__)
    // macOS doesn't have aligned_alloc before 10.15
    if (posix_memalign(&ptr, alignment, sz) != 0) {
      ptr = nullptr;
    }
#elif defined(__linux__)
    ensure_real_functions();
    if (g_initializing) {
      // During dlsym, use init buffer (ignoring alignment for simplicity)
      ptr = init_buffer_alloc(sz);
    } else if (real_aligned_alloc) {
      ptr = real_aligned_alloc(alignment, sz);
    } else {
      ptr = aligned_alloc(alignment, sz);
    }
#else
    ptr = aligned_alloc(alignment, sz);
#endif

    if (ptr) {
      g_totalAllocated += sz;
      g_allocCount++;
    }

    return ptr;
  }

  size_t getSize(void* ptr) {
    if (!ptr) return 0;

#if defined(_WIN32)
    return _msize(ptr);
#elif defined(__APPLE__)
    return malloc_size(ptr);
#elif defined(__linux__)
    // Init buffer allocations - return a reasonable size
    if (is_init_buffer_ptr(ptr)) {
      return 64; // Conservative estimate
    }
    return real_malloc_usable_size ? real_malloc_usable_size(ptr) : malloc_usable_size(ptr);
#else
    return malloc_usable_size(ptr);
#endif
  }

  void lock() {
    mutex_.lock();
  }

  void unlock() {
    mutex_.unlock();
  }
};

// ─── GENERATE XXMALLOC INTERFACE ──────────────────────────────────────────────

using SimpleHeapRedirect = alloc8::HeapRedirect<SimpleHeap>;
ALLOC8_REDIRECT(SimpleHeapRedirect);

// ─── STATISTICS REPORTING ─────────────────────────────────────────────────────

// Print stats at program exit
static void printStats() {
  fprintf(stderr, "\n=== SimpleHeap Statistics ===\n");
  fprintf(stderr, "Total allocated: %zu bytes\n", g_totalAllocated.load());
  fprintf(stderr, "Total freed:     %zu bytes\n", g_totalFreed.load());
  fprintf(stderr, "Net usage:       %zu bytes\n",
          g_totalAllocated.load() - g_totalFreed.load());
  fprintf(stderr, "Peak usage:      %zu bytes\n", g_peakUsage.load());
  fprintf(stderr, "Alloc count:     %zu\n", g_allocCount.load());
  fprintf(stderr, "Free count:      %zu\n", g_freeCount.load());
  fprintf(stderr, "=============================\n");
}

// Register printStats to run at program exit
#if defined(_WIN32)
// Windows: Use DllMain or a static initializer
namespace {
  struct StatsPrinter {
    ~StatsPrinter() { printStats(); }
  };
  static StatsPrinter g_statsPrinter;
}
#else
// Unix: Use constructor attribute to register atexit handler
__attribute__((constructor))
static void registerPrintStats() {
  atexit(printStats);
}
#endif

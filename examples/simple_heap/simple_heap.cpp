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
    // Use system malloc as backing allocator
    void* ptr = std::malloc(sz);

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

    size_t sz = getSize(ptr);
    g_totalFreed += sz;
    g_freeCount++;

    std::free(ptr);
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
__attribute__((destructor))
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

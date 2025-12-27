// alloc8/function_table.h - Runtime function pointer API
#pragma once

#include "platform.h"
#include <cstddef>

namespace alloc8 {

// ─── FUNCTION TABLE STRUCTURE ─────────────────────────────────────────────────

/**
 * Function pointer table for runtime allocator selection.
 *
 * Use this when you need to:
 * - Swap allocators at runtime
 * - The allocator isn't known at compile time
 * - Interface with C code that can't use templates
 *
 * Note: This has slightly more overhead than the template approach due to
 * indirect function calls that can't be inlined.
 */
struct AllocatorFunctionTable {
  void* (*malloc)(size_t size);
  void  (*free)(void* ptr);
  void* (*realloc)(void* ptr, size_t size);
  void* (*calloc)(size_t count, size_t size);
  void* (*memalign)(size_t alignment, size_t size);
  size_t (*malloc_usable_size)(void* ptr);
  void  (*lock)();
  void  (*unlock)();

  // Optional context pointer for allocators that need state
  void* context;
};

// ─── TABLE CREATION FROM HEAPREDIRECT ─────────────────────────────────────────

/**
 * Create a function table from a HeapRedirect type at compile time.
 *
 * Example:
 *   using MyRedirect = alloc8::HeapRedirect<MyHeap>;
 *   auto table = alloc8::makeAllocatorTable<MyRedirect>();
 */
template<typename HeapRedirectType>
constexpr AllocatorFunctionTable makeAllocatorTable() {
  return AllocatorFunctionTable{
    .malloc = [](size_t sz) -> void* {
      return HeapRedirectType::malloc(sz);
    },
    .free = [](void* ptr) {
      HeapRedirectType::free(ptr);
    },
    .realloc = [](void* ptr, size_t sz) -> void* {
      return HeapRedirectType::realloc(ptr, sz);
    },
    .calloc = [](size_t n, size_t sz) -> void* {
      return HeapRedirectType::calloc(n, sz);
    },
    .memalign = [](size_t align, size_t sz) -> void* {
      return HeapRedirectType::memalign(align, sz);
    },
    .malloc_usable_size = [](void* ptr) -> size_t {
      return HeapRedirectType::getSize(ptr);
    },
    .lock = []() {
      HeapRedirectType::lock();
    },
    .unlock = []() {
      HeapRedirectType::unlock();
    },
    .context = nullptr,
  };
}

// ─── GLOBAL TABLE FOR RUNTIME DISPATCH ────────────────────────────────────────

/**
 * Global function table pointer for runtime switching.
 *
 * Set this before any allocations if you want to use runtime dispatch.
 * Default is nullptr; you must initialize it.
 *
 * Example:
 *   static auto myTable = alloc8::makeAllocatorTable<MyRedirect>();
 *   alloc8::g_allocator_table = &myTable;
 */
inline AllocatorFunctionTable* g_allocator_table = nullptr;

// ─── RUNTIME DISPATCH FUNCTIONS ───────────────────────────────────────────────

/**
 * Runtime dispatch versions.
 * Use sparingly - prefer templates for hot paths.
 */
ALLOC8_ALWAYS_INLINE
inline void* rt_malloc(size_t sz) {
  return g_allocator_table->malloc(sz);
}

ALLOC8_ALWAYS_INLINE
inline void rt_free(void* ptr) {
  g_allocator_table->free(ptr);
}

ALLOC8_ALWAYS_INLINE
inline void* rt_realloc(void* ptr, size_t sz) {
  return g_allocator_table->realloc(ptr, sz);
}

ALLOC8_ALWAYS_INLINE
inline void* rt_calloc(size_t count, size_t sz) {
  return g_allocator_table->calloc(count, sz);
}

ALLOC8_ALWAYS_INLINE
inline void* rt_memalign(size_t alignment, size_t sz) {
  return g_allocator_table->memalign(alignment, sz);
}

ALLOC8_ALWAYS_INLINE
inline size_t rt_malloc_usable_size(void* ptr) {
  return g_allocator_table->malloc_usable_size(ptr);
}

} // namespace alloc8

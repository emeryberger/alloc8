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

void* replace_malloc(size_t sz) {
  return xxmalloc(sz);
}

void replace_free(void* ptr) {
  xxfree(ptr);
}

size_t replace_malloc_usable_size(void* ptr) {
  if (!ptr) return 0;
  return xxmalloc_usable_size(ptr);
}

size_t replace_malloc_good_size(size_t sz) {
  return sz ? sz : 1;
}

void* replace_realloc(void* ptr, size_t sz) {
  // NULL ptr = malloc
  if (!ptr) {
    return xxmalloc(sz);
  }

  // 0 size = free (macOS returns small allocation)
  if (sz == 0) {
    xxfree(ptr);
    return xxmalloc(1);
  }

  size_t oldSize = xxmalloc_usable_size(ptr);

  // Don't reallocate if shrinking by less than half
  if ((oldSize / 2 < sz) && (sz <= oldSize)) {
    return ptr;
  }

  void* newPtr = xxmalloc(sz);
  if (newPtr) {
    size_t copySize = (oldSize < sz) ? oldSize : sz;
    memcpy(newPtr, ptr, copySize);
    xxfree(ptr);
  }

  return newPtr;
}

// macOS-specific reallocf - frees original on failure
void* replace_reallocf(void* ptr, size_t sz) {
  if (!ptr) {
    return xxmalloc(sz);
  }

  if (sz == 0) {
    xxfree(ptr);
    return xxmalloc(1);
  }

  size_t oldSize = xxmalloc_usable_size(ptr);

  if ((oldSize / 2 < sz) && (sz <= oldSize)) {
    return ptr;
  }

  void* newPtr = xxmalloc(sz);
  if (newPtr) {
    size_t copySize = (oldSize < sz) ? oldSize : sz;
    memcpy(newPtr, ptr, copySize);
  }
  // reallocf always frees the original
  xxfree(ptr);

  return newPtr;
}

void* replace_calloc(size_t count, size_t size) {
  return xxcalloc(count, size);
}

char* replace_strdup(const char* s) {
  if (!s) return nullptr;
  size_t len = strlen(s) + 1;
  char* newStr = (char*)xxmalloc(len);
  if (newStr) {
    memcpy(newStr, s, len);
  }
  return newStr;
}

void* replace_memalign(size_t alignment, size_t size) {
  return xxmemalign(alignment, size);
}

void* replace_aligned_alloc(size_t alignment, size_t size) {
  if (alignment == 0 || (size % alignment) != 0) {
    return nullptr;
  }
  return xxmemalign(alignment, size);
}

int replace_posix_memalign(void** memptr, size_t alignment, size_t size) {
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
  *memptr = ptr;
  return 0;
}

void* replace_valloc(size_t sz) {
  return xxmemalign(ALLOC8_PAGE_SIZE, sz);
}

void replace_vfree(void* ptr) {
  xxfree(ptr);
}

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

// Core allocation functions
MAC_INTERPOSE(replace_malloc, malloc);
MAC_INTERPOSE(xxfree, free);
MAC_INTERPOSE(replace_calloc, calloc);
MAC_INTERPOSE(replace_realloc, realloc);
MAC_INTERPOSE(replace_reallocf, reallocf);
// Note: memalign doesn't exist on macOS, only posix_memalign
MAC_INTERPOSE(replace_aligned_alloc, aligned_alloc);
MAC_INTERPOSE(replace_posix_memalign, posix_memalign);
MAC_INTERPOSE(replace_valloc, valloc);
MAC_INTERPOSE(replace_vfree, vfree);
MAC_INTERPOSE(replace_strdup, strdup);
MAC_INTERPOSE(xxmalloc_usable_size, malloc_size);
MAC_INTERPOSE(replace_malloc_good_size, malloc_good_size);
MAC_INTERPOSE(replace_malloc_printf, malloc_printf);

// Fork handlers
MAC_INTERPOSE(replace__malloc_fork_prepare, _malloc_fork_prepare);
MAC_INTERPOSE(replace__malloc_fork_parent, _malloc_fork_parent);
MAC_INTERPOSE(replace__malloc_fork_child, _malloc_fork_child);

// C++ operators - use xxmalloc/xxfree directly for performance
// operator new(size_t)
MAC_INTERPOSE(xxmalloc, _Znwm);
// operator new[](size_t)
MAC_INTERPOSE(xxmalloc, _Znam);
// operator delete(void*)
MAC_INTERPOSE(xxfree, _ZdlPv);
// operator delete[](void*)
MAC_INTERPOSE(xxfree, _ZdaPv);
// operator new(size_t, nothrow)
MAC_INTERPOSE(xxmalloc, _ZnwmRKSt9nothrow_t);
// operator new[](size_t, nothrow)
MAC_INTERPOSE(xxmalloc, _ZnamRKSt9nothrow_t);
// operator delete(void*, nothrow)
MAC_INTERPOSE(xxfree, _ZdlPvRKSt9nothrow_t);
// operator delete[](void*, nothrow)
MAC_INTERPOSE(xxfree, _ZdaPvRKSt9nothrow_t);

// Malloc zone functions
MAC_INTERPOSE(replace_malloc_create_zone, malloc_create_zone);
MAC_INTERPOSE(replace_malloc_default_zone, malloc_default_zone);
MAC_INTERPOSE(replace_malloc_default_purgeable_zone, malloc_default_purgeable_zone);
MAC_INTERPOSE(replace_malloc_destroy_zone, malloc_destroy_zone);
MAC_INTERPOSE(replace_malloc_get_all_zones, malloc_get_all_zones);
MAC_INTERPOSE(replace_malloc_get_zone_name, malloc_get_zone_name);
MAC_INTERPOSE(replace_malloc_set_zone_name, malloc_set_zone_name);
MAC_INTERPOSE(replace_malloc_zone_batch_malloc, malloc_zone_batch_malloc);
MAC_INTERPOSE(replace_malloc_zone_batch_free, malloc_zone_batch_free);
MAC_INTERPOSE(replace_malloc_zone_calloc, malloc_zone_calloc);
MAC_INTERPOSE(replace_malloc_zone_check, malloc_zone_check);
MAC_INTERPOSE(replace_malloc_zone_free, malloc_zone_free);
MAC_INTERPOSE(replace_malloc_zone_from_ptr, malloc_zone_from_ptr);
MAC_INTERPOSE(replace_malloc_zone_log, malloc_zone_log);
MAC_INTERPOSE(replace_malloc_zone_malloc, malloc_zone_malloc);
MAC_INTERPOSE(replace_malloc_zone_memalign, malloc_zone_memalign);
MAC_INTERPOSE(replace_malloc_zone_print, malloc_zone_print);
MAC_INTERPOSE(replace_malloc_zone_print_ptr_info, malloc_zone_print_ptr_info);
MAC_INTERPOSE(replace_malloc_zone_realloc, malloc_zone_realloc);
MAC_INTERPOSE(replace_malloc_zone_register, malloc_zone_register);
MAC_INTERPOSE(replace_malloc_zone_unregister, malloc_zone_unregister);
MAC_INTERPOSE(replace_malloc_zone_valloc, malloc_zone_valloc);

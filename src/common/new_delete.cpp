// alloc8/src/common/new_delete.cpp
// C++ operator new/delete replacements
//
// This file provides standalone operator new/delete that can be linked
// into allocator libraries. For platform wrappers that need to include
// the operators inline (like gnu_wrapper.cpp), use new_delete.inc instead.

#include <alloc8/alloc8.h>
#include <new>
#include <cstdlib>

extern "C" {
  void* xxmalloc(size_t);
  void  xxfree(void*);
  void* xxmemalign(size_t, size_t);
}

// ─── THROWING VARIANTS ────────────────────────────────────────────────────────

ALLOC8_EXPORT void* operator new(std::size_t sz) {
  void* ptr = xxmalloc(sz);
  if (ALLOC8_UNLIKELY(!ptr)) {
    throw std::bad_alloc();
  }
  return ptr;
}

ALLOC8_EXPORT void* operator new[](std::size_t sz) {
  void* ptr = xxmalloc(sz);
  if (ALLOC8_UNLIKELY(!ptr)) {
    throw std::bad_alloc();
  }
  return ptr;
}

// ─── NON-THROWING VARIANTS ────────────────────────────────────────────────────

ALLOC8_EXPORT void* operator new(std::size_t sz, const std::nothrow_t&) noexcept {
  return xxmalloc(sz);
}

ALLOC8_EXPORT void* operator new[](std::size_t sz, const std::nothrow_t&) noexcept {
  return xxmalloc(sz);
}

// ─── DELETE OPERATORS ─────────────────────────────────────────────────────────

ALLOC8_EXPORT void operator delete(void* ptr) noexcept {
  if (ptr) xxfree(ptr);
}

ALLOC8_EXPORT void operator delete[](void* ptr) noexcept {
  if (ptr) xxfree(ptr);
}

ALLOC8_EXPORT void operator delete(void* ptr, const std::nothrow_t&) noexcept {
  if (ptr) xxfree(ptr);
}

ALLOC8_EXPORT void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  if (ptr) xxfree(ptr);
}

// ─── SIZED DELETE (C++14) ─────────────────────────────────────────────────────

#if defined(__cpp_sized_deallocation) && __cpp_sized_deallocation >= 201309L

ALLOC8_EXPORT void operator delete(void* ptr, std::size_t) noexcept {
  if (ptr) xxfree(ptr);
}

ALLOC8_EXPORT void operator delete[](void* ptr, std::size_t) noexcept {
  if (ptr) xxfree(ptr);
}

#endif // sized deallocation

// ─── ALIGNED NEW/DELETE (C++17) ────────────────────────────────────────────────

#if defined(__cpp_aligned_new) && __cpp_aligned_new >= 201606L

ALLOC8_EXPORT void* operator new(std::size_t sz, std::align_val_t al) {
  void* ptr = xxmemalign(static_cast<std::size_t>(al), sz);
  if (ALLOC8_UNLIKELY(!ptr)) {
    throw std::bad_alloc();
  }
  return ptr;
}

ALLOC8_EXPORT void* operator new[](std::size_t sz, std::align_val_t al) {
  void* ptr = xxmemalign(static_cast<std::size_t>(al), sz);
  if (ALLOC8_UNLIKELY(!ptr)) {
    throw std::bad_alloc();
  }
  return ptr;
}

ALLOC8_EXPORT void* operator new(std::size_t sz, std::align_val_t al, const std::nothrow_t&) noexcept {
  return xxmemalign(static_cast<std::size_t>(al), sz);
}

ALLOC8_EXPORT void* operator new[](std::size_t sz, std::align_val_t al, const std::nothrow_t&) noexcept {
  return xxmemalign(static_cast<std::size_t>(al), sz);
}

ALLOC8_EXPORT void operator delete(void* ptr, std::align_val_t) noexcept {
  if (ptr) xxfree(ptr);
}

ALLOC8_EXPORT void operator delete[](void* ptr, std::align_val_t) noexcept {
  if (ptr) xxfree(ptr);
}

ALLOC8_EXPORT void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  if (ptr) xxfree(ptr);
}

ALLOC8_EXPORT void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  if (ptr) xxfree(ptr);
}

// Sized + aligned delete
#if defined(__cpp_sized_deallocation) && __cpp_sized_deallocation >= 201309L

ALLOC8_EXPORT void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
  if (ptr) xxfree(ptr);
}

ALLOC8_EXPORT void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
  if (ptr) xxfree(ptr);
}

#endif // sized + aligned

#endif // aligned new

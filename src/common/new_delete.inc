// alloc8/src/common/new_delete.inc
// C++ operator new/delete - include file for platform wrappers
//
// This file is designed to be #included by platform wrapper .cpp files
// (e.g., gnu_wrapper.cpp) so that the operators are compiled into the
// same translation unit with appropriate attributes.

#include <new>
#include <cstdlib>

// Expects xxmalloc, xxfree, xxmemalign to be declared

// ─── THROWING VARIANTS ────────────────────────────────────────────────────────

ATTRIBUTE_EXPORT __attribute__((flatten))
void* operator new(std::size_t sz) {
  void* ptr = xxmalloc(sz);
  if (!ptr) {
    throw std::bad_alloc();
  }
  return ptr;
}

ATTRIBUTE_EXPORT __attribute__((flatten))
void* operator new[](std::size_t sz) {
  void* ptr = xxmalloc(sz);
  if (!ptr) {
    throw std::bad_alloc();
  }
  return ptr;
}

// ─── NON-THROWING VARIANTS ────────────────────────────────────────────────────

ATTRIBUTE_EXPORT __attribute__((flatten))
void* operator new(std::size_t sz, const std::nothrow_t&) noexcept {
  return xxmalloc(sz);
}

ATTRIBUTE_EXPORT __attribute__((flatten))
void* operator new[](std::size_t sz, const std::nothrow_t&) noexcept {
  return xxmalloc(sz);
}

// ─── DELETE OPERATORS ─────────────────────────────────────────────────────────

ATTRIBUTE_EXPORT __attribute__((flatten))
void operator delete(void* ptr) noexcept {
  if (ptr) xxfree(ptr);
}

ATTRIBUTE_EXPORT __attribute__((flatten))
void operator delete[](void* ptr) noexcept {
  if (ptr) xxfree(ptr);
}

ATTRIBUTE_EXPORT __attribute__((flatten))
void operator delete(void* ptr, const std::nothrow_t&) noexcept {
  if (ptr) xxfree(ptr);
}

ATTRIBUTE_EXPORT __attribute__((flatten))
void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  if (ptr) xxfree(ptr);
}

// ─── SIZED DELETE (C++14) ─────────────────────────────────────────────────────

#if defined(__cpp_sized_deallocation) && __cpp_sized_deallocation >= 201309L

ATTRIBUTE_EXPORT __attribute__((flatten))
void operator delete(void* ptr, std::size_t) noexcept {
  if (ptr) xxfree(ptr);
}

ATTRIBUTE_EXPORT __attribute__((flatten))
void operator delete[](void* ptr, std::size_t) noexcept {
  if (ptr) xxfree(ptr);
}

#endif

// ─── ALIGNED NEW/DELETE (C++17) ────────────────────────────────────────────────

#if defined(__cpp_aligned_new) && __cpp_aligned_new >= 201606L

ATTRIBUTE_EXPORT __attribute__((flatten))
void* operator new(std::size_t sz, std::align_val_t al) {
  void* ptr = xxmemalign(static_cast<std::size_t>(al), sz);
  if (!ptr) {
    throw std::bad_alloc();
  }
  return ptr;
}

ATTRIBUTE_EXPORT __attribute__((flatten))
void* operator new[](std::size_t sz, std::align_val_t al) {
  void* ptr = xxmemalign(static_cast<std::size_t>(al), sz);
  if (!ptr) {
    throw std::bad_alloc();
  }
  return ptr;
}

ATTRIBUTE_EXPORT __attribute__((flatten))
void* operator new(std::size_t sz, std::align_val_t al, const std::nothrow_t&) noexcept {
  return xxmemalign(static_cast<std::size_t>(al), sz);
}

ATTRIBUTE_EXPORT __attribute__((flatten))
void* operator new[](std::size_t sz, std::align_val_t al, const std::nothrow_t&) noexcept {
  return xxmemalign(static_cast<std::size_t>(al), sz);
}

ATTRIBUTE_EXPORT __attribute__((flatten))
void operator delete(void* ptr, std::align_val_t) noexcept {
  if (ptr) xxfree(ptr);
}

ATTRIBUTE_EXPORT __attribute__((flatten))
void operator delete[](void* ptr, std::align_val_t) noexcept {
  if (ptr) xxfree(ptr);
}

ATTRIBUTE_EXPORT __attribute__((flatten))
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  if (ptr) xxfree(ptr);
}

ATTRIBUTE_EXPORT __attribute__((flatten))
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  if (ptr) xxfree(ptr);
}

// Sized + aligned delete
#if defined(__cpp_sized_deallocation) && __cpp_sized_deallocation >= 201309L

ATTRIBUTE_EXPORT __attribute__((flatten))
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
  if (ptr) xxfree(ptr);
}

ATTRIBUTE_EXPORT __attribute__((flatten))
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
  if (ptr) xxfree(ptr);
}

#endif // sized + aligned

#endif // aligned new

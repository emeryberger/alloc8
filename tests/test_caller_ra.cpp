// alloc8/tests/test_caller_ra.cpp
// Verifies the caller return-address hint (alloc8_caller_ra): wrapper entries
// store __builtin_return_address(0) before dispatching to xx*, so an allocator
// can key routing decisions on the application's allocation site.
//
// Links new_delete.inc directly (portable across Linux/macOS/Windows) with
// stub xx* functions that snapshot the hint. Two distinct noinline call sites
// must observe two distinct, non-null return addresses, each pointing into
// this test binary's own code.

#include <alloc8/alloc8.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define ATTRIBUTE_EXPORT

static void* g_last_hint = nullptr;

extern "C" {
void* xxmalloc(size_t sz) {
  g_last_hint = alloc8_caller_ra;
  return ::malloc(sz);
}
void xxfree(void* p) { ::free(p); }
void* xxmemalign(size_t alignment, size_t sz) {
  g_last_hint = alloc8_caller_ra;
#if defined(_WIN32)
  return ::_aligned_malloc(sz, alignment);
#else
  void* p = nullptr;
  if (::posix_memalign(&p, alignment, sz) != 0) return nullptr;
  return p;
#endif
}
void xxfree_sized(void* p, size_t) { ::free(p); }
void xxfree_aligned_sized(void* p, size_t, size_t) { ::free(p); }
}

// Replace this test's global operator new/delete with the alloc8 wrappers
// under test (they call ALLOC8_SET_CALLER_RA then dispatch to the stubs).
#include "../src/common/new_delete.inc"

// Two distinct allocation sites. noinline + a volatile sink keeps each call
// a genuine call instruction with its own return address.
static volatile void* g_sink;

// Bodies deliberately differ (allocation size) so identical-code folding can
// never merge the two sites into one address.
__attribute__((noinline)) static void* site_a() {
  void* p = new char[64];
  g_sink = p;
  return p;
}

__attribute__((noinline)) static void* site_b() {
  void* p = new char[128];
  g_sink = p;
  return p;
}

int main() {
  void* pa = site_a();
  void* ra_a = g_last_hint;

  void* pb = site_b();
  void* ra_b = g_last_hint;

  delete[] static_cast<char*>(pa);
  delete[] static_cast<char*>(pb);

  if (!ra_a || !ra_b) {
    fprintf(stderr, "FAIL: hint not set (a=%p b=%p)\n", ra_a, ra_b);
    return 1;
  }
  if (ra_a == ra_b) {
    fprintf(stderr, "FAIL: distinct sites saw identical hint %p\n", ra_a);
    return 1;
  }
  // Both hints must land inside this binary's code, near their site functions.
  // (Within 4 KB is generous; a hint pointing into a wrapper or libc would be
  // megabytes away or in another image.)
  auto near = [](void* hint, void* fn) {
    auto h = reinterpret_cast<uintptr_t>(hint);
    auto f = reinterpret_cast<uintptr_t>(fn);
    return (h > f ? h - f : f - h) < 4096;
  };
  if (!near(ra_a, reinterpret_cast<void*>(&site_a)) ||
      !near(ra_b, reinterpret_cast<void*>(&site_b))) {
    fprintf(stderr, "FAIL: hint outside caller (a=%p site_a=%p, b=%p site_b=%p)\n",
            ra_a, reinterpret_cast<void*>(&site_a),
            ra_b, reinterpret_cast<void*>(&site_b));
    return 1;
  }
  printf("PASSED: caller-RA hint distinct per site (a=%p b=%p)\n", ra_a, ra_b);
  return 0;
}

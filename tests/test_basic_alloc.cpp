// alloc8/tests/test_basic_alloc.cpp
// Basic allocation tests

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cassert>

// Simple test macro
#define TEST(name) \
  static void test_##name(); \
  static struct Test_##name { \
    Test_##name() { \
      printf("Running %s... ", #name); \
      test_##name(); \
      printf("PASSED\n"); \
    } \
  } test_##name##_instance; \
  static void test_##name()

// ─── TESTS ────────────────────────────────────────────────────────────────────

TEST(malloc_free_basic) {
  void* p = malloc(100);
  assert(p != nullptr);
  memset(p, 0xAB, 100);
  free(p);
}

TEST(malloc_zero) {
  // malloc(0) should return a valid pointer or NULL
  void* p = malloc(0);
  // Either is valid per POSIX
  free(p);  // free(NULL) is safe
}

TEST(calloc_zeroed) {
  int* p = (int*)calloc(10, sizeof(int));
  assert(p != nullptr);
  for (int i = 0; i < 10; i++) {
    assert(p[i] == 0);
  }
  free(p);
}

TEST(calloc_overflow) {
  // Should return NULL on overflow
  void* p = calloc(SIZE_MAX, SIZE_MAX);
  assert(p == nullptr);
}

TEST(realloc_null) {
  // realloc(NULL, size) = malloc(size)
  void* p = realloc(nullptr, 100);
  assert(p != nullptr);
  free(p);
}

TEST(realloc_grow) {
  char* p = (char*)malloc(100);
  assert(p != nullptr);
  memset(p, 'A', 100);

  p = (char*)realloc(p, 1000);
  assert(p != nullptr);

  // First 100 bytes should be preserved
  for (int i = 0; i < 100; i++) {
    assert(p[i] == 'A');
  }

  free(p);
}

TEST(realloc_shrink) {
  char* p = (char*)malloc(1000);
  assert(p != nullptr);
  memset(p, 'B', 1000);

  p = (char*)realloc(p, 100);
  assert(p != nullptr);

  // First 100 bytes should be preserved
  for (int i = 0; i < 100; i++) {
    assert(p[i] == 'B');
  }

  free(p);
}

TEST(free_null) {
  // free(NULL) should be safe
  free(nullptr);
}

TEST(malloc_large) {
  // Allocate 10 MB
  size_t size = 10 * 1024 * 1024;
  void* p = malloc(size);
  assert(p != nullptr);
  memset(p, 0xCD, size);
  free(p);
}

TEST(malloc_many_small) {
  const int count = 10000;
  void* ptrs[count];

  for (int i = 0; i < count; i++) {
    ptrs[i] = malloc(32);
    assert(ptrs[i] != nullptr);
  }

  for (int i = 0; i < count; i++) {
    free(ptrs[i]);
  }
}

#if !defined(_WIN32)
// posix_memalign is not available on Windows
TEST(memalign_basic) {
  void* p = nullptr;
  int result = posix_memalign(&p, 64, 100);
  assert(result == 0);
  assert(p != nullptr);
  assert(((size_t)p % 64) == 0);
  free(p);
}

TEST(memalign_page) {
  void* p = nullptr;
  int result = posix_memalign(&p, 4096, 4096);
  assert(result == 0);
  assert(p != nullptr);
  assert(((size_t)p % 4096) == 0);
  free(p);
}
#else
// Windows: Use _aligned_malloc instead
TEST(memalign_basic) {
  void* p = _aligned_malloc(100, 64);
  assert(p != nullptr);
  assert(((size_t)p % 64) == 0);
  _aligned_free(p);
}

TEST(memalign_page) {
  void* p = _aligned_malloc(4096, 4096);
  assert(p != nullptr);
  assert(((size_t)p % 4096) == 0);
  _aligned_free(p);
}
#endif

TEST(strdup_basic) {
  const char* original = "Hello, World!";
  char* copy = strdup(original);
  assert(copy != nullptr);
  assert(strcmp(original, copy) == 0);
  free(copy);
}

// ─── MAIN ─────────────────────────────────────────────────────────────────────

int main() {
  printf("\n=== alloc8 Basic Allocation Tests ===\n\n");
  // Tests are run automatically via static constructors
  printf("\n=== All tests passed! ===\n\n");
  return 0;
}

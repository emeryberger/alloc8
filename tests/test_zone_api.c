// macOS zone-API coverage for alloc8's default-zone takeover.
//
// Run under interposition (DYLD_INSERT_LIBRARIES=<allocator built on alloc8>).
// Allocations made through the zone API must reach the user allocator, and
// every mix of zone/plain alloc and free must stay consistent:
//   - malloc_zone_malloc(malloc_default_zone()) — what SQLite does on macOS
//   - plain malloc freed with malloc_zone_free, and vice versa
//   - realloc/calloc/memalign/batch through the default zone
//   - private zones from malloc_create_zone
//   - CoreFoundation objects (allocate through zones internally)
//   - libSystem-internal allocation (strdup, asprintf) freed by the app
//   - fork(): libmalloc locks every registered zone around fork
// With ALLOC8_EXPECT_ZONE=1, also asserts that default-zone allocations are
// attributed to alloc8's zone (i.e. the takeover actually happened).

#include <CoreFoundation/CoreFoundation.h>
#include <malloc/malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, ...)                                          \
  do {                                                            \
    if (!(cond)) {                                                \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);         \
      fprintf(stderr, __VA_ARGS__);                               \
      fputc('\n', stderr);                                        \
      ++failures;                                                 \
    }                                                             \
  } while (0)

static int is_alloc8_zone(const void* p) {
  malloc_zone_t* z = malloc_zone_from_ptr(p);
  const char* name = z ? malloc_get_zone_name(z) : NULL;
  return name && strcmp(name, "alloc8DefaultZone") == 0;
}

int main(void) {
  int expect_zone = getenv("ALLOC8_EXPECT_ZONE") && getenv("ALLOC8_EXPECT_ZONE")[0] == '1';
  malloc_zone_t* dz = malloc_default_zone();

  // 1. Default-zone allocation, freed both ways.
  for (size_t sz = 1; sz <= (4u << 20); sz *= 3) {
    char* p = malloc_zone_malloc(dz, sz);
    CHECK(p != NULL, "zone malloc %zu", sz);
    memset(p, 0xAB, sz);
    CHECK(malloc_size(p) >= sz, "malloc_size %zu < %zu", malloc_size(p), sz);
    CHECK(malloc_zone_from_ptr(p) != NULL, "no zone for zone-malloc'd %zu", sz);
    if (expect_zone) CHECK(is_alloc8_zone(p), "zone malloc %zu not in alloc8 zone", sz);
    if (sz & 1) free(p); else malloc_zone_free(dz, p);

    char* q = malloc(sz);
    memset(q, 0xCD, sz);
    // Without the takeover, malloc_zone_from_ptr() returns NULL for pointers
    // the user allocator issued through plain malloc (no zone owns them).
    malloc_zone_t* qz = malloc_zone_from_ptr(q);
    if (expect_zone) CHECK(qz != NULL && is_alloc8_zone(q), "malloc'd %zu not in alloc8 zone", sz);
    if ((sz & 1) && qz) malloc_zone_free(qz, q); else free(q);
  }

  // 2. calloc / realloc / memalign / valloc through the default zone.
  int* c = malloc_zone_calloc(dz, 1000, sizeof(int));
  int zero = 1;
  for (int i = 0; i < 1000; ++i) zero &= (c[i] == 0);
  CHECK(zero, "zone calloc not zeroed");
  for (int i = 0; i < 1000; ++i) c[i] = i;
  c = malloc_zone_realloc(dz, c, 100000 * sizeof(int));
  int kept = 1;
  for (int i = 0; i < 1000; ++i) kept &= (c[i] == i);
  CHECK(kept, "zone realloc lost data");
  c = realloc(c, 10 * sizeof(int));       // plain realloc of a zone pointer
  CHECK(c && c[9] == 9, "plain realloc of zone pointer lost data");
  free(c);
  void* m = malloc_zone_memalign(dz, 4096, 10000);
  CHECK(m && ((uintptr_t)m & 4095) == 0, "zone memalign misaligned");
  malloc_zone_free(dz, m);
  void* v = malloc_zone_valloc(dz, 100);
  CHECK(v && ((uintptr_t)v % (uintptr_t)getpagesize()) == 0, "zone valloc misaligned");
  free(v);

  // 3. Batch API.
  void* batch[64];
  unsigned got = malloc_zone_batch_malloc(dz, 48, batch, 64);
  CHECK(got > 0, "batch malloc returned nothing");
  for (unsigned i = 0; i < got; ++i) memset(batch[i], (int)i, 48);
  malloc_zone_batch_free(dz, batch, got);

  // 4. A private zone keeps working (libSystem-owned; we only take the default).
  malloc_zone_t* priv = malloc_create_zone(0, 0);
  char* pp = malloc_zone_malloc(priv, 777);
  memset(pp, 1, 777);
  CHECK(malloc_zone_from_ptr(pp) == priv, "private-zone pointer attributed elsewhere");
  free(pp);  // plain free must find the private zone
  malloc_destroy_zone(priv);

  // 5. libSystem-internal allocations freed by the app.
  char* d = strdup("hello zones");
  CHECK(d && strcmp(d, "hello zones") == 0, "strdup");
  free(d);
  char* s = NULL;
  CHECK(asprintf(&s, "%d-%s", 42, "x") > 0 && strcmp(s, "42-x") == 0, "asprintf");
  free(s);

  // 6. CoreFoundation.
  CFMutableArrayRef arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
  for (int i = 0; i < 2000; ++i) {
    CFStringRef str = CFStringCreateWithFormat(NULL, NULL, CFSTR("item-%d-%s"), i,
                                               "padding padding padding padding");
    CFArrayAppendValue(arr, str);
    CFRelease(str);
  }
  CHECK(CFArrayGetCount(arr) == 2000, "CFArray count");
  CFStringRef joined = CFStringCreateByCombiningStrings(NULL, arr, CFSTR(","));
  CHECK(CFStringGetLength(joined) > 2000 * 10, "CFString join");
  CFRelease(joined);
  CFRelease(arr);

  // 7. fork(): libmalloc force_lock/force_unlock/reinit_lock on every zone.
  pid_t pid = fork();
  if (pid == 0) {
    char* cp = malloc_zone_malloc(malloc_default_zone(), 1234);
    memset(cp, 2, 1234);
    free(cp);
    char* cq = strdup("child");
    free(cq);
    _exit(cq ? 0 : 1);
  }
  int status = 0;
  CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0,
        "forked child failed (status %d)", status);

  if (failures) {
    fprintf(stderr, "test_zone_api: %d failure(s)\n", failures);
    return 1;
  }
  printf("test_zone_api: OK%s\n", expect_zone ? " (alloc8 zone is default)" : "");
  return 0;
}

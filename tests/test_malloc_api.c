/*
 * test_malloc_api.c - Comprehensive malloc API test for alloc8
 *
 * Tests all standard allocation functions to ensure correct interposition
 * and behavior across platforms. This is a correctness test, not a benchmark.
 *
 * Build: gcc -o test_malloc_api test_malloc_api.c -lpthread
 * Run:   LD_PRELOAD=./libyouralloc.so ./test_malloc_api
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <malloc.h>
#define posix_memalign_impl(p, a, s) ((*(p) = _aligned_malloc((s), (a))) ? 0 : ENOMEM)
#define aligned_alloc_impl(a, s) _aligned_malloc((s), (a))
#define aligned_free_impl(p) _aligned_free(p)
#define memalign_impl(a, s) _aligned_malloc((s), (a))
#define valloc_impl(s) _aligned_malloc((s), 4096)
#define malloc_usable_size_impl(p) _msize(p)
#define HAS_STRNDUP 0
#define HAS_MEMALIGN 0
#define HAS_VALLOC 0
#elif defined(__APPLE__)
#include <unistd.h>
#include <malloc/malloc.h>
#define posix_memalign_impl posix_memalign
#define aligned_alloc_impl aligned_alloc
#define aligned_free_impl free
/* macOS doesn't have memalign or valloc in modern SDKs */
#define HAS_MEMALIGN 0
#define HAS_VALLOC 0
#define malloc_usable_size_impl malloc_size
#define HAS_STRNDUP 1
#else
/* Linux/BSD */
#include <unistd.h>
#include <malloc.h>
#define posix_memalign_impl posix_memalign
#define aligned_alloc_impl aligned_alloc
#define aligned_free_impl free
#define memalign_impl memalign
#define valloc_impl valloc
#define malloc_usable_size_impl malloc_usable_size
#define HAS_STRNDUP 1
#define HAS_MEMALIGN 1
#define HAS_VALLOC 1
#endif

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  %-55s ", name); \
    fflush(stdout); \
} while(0)

#define PASS() do { \
    tests_passed++; \
    printf("[PASS]\n"); \
} while(0)

#define FAIL(msg) do { \
    tests_failed++; \
    printf("[FAIL] %s\n", msg); \
} while(0)

/* ═══════════════════════════════════════════════════════════════════════════
 * BASIC MALLOC/FREE TESTS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_malloc_free_basic(void) {
    TEST("malloc/free basic");
    void *p = malloc(100);
    if (p == NULL) { FAIL("malloc returned NULL"); return; }
    memset(p, 0xAB, 100);
    free(p);
    PASS();
}

static void test_malloc_sizes(void) {
    TEST("malloc various sizes (1B to 4MB)");
    size_t sizes[] = {1, 7, 8, 15, 16, 31, 32, 64, 128, 256, 512, 1024,
                      2048, 4096, 8192, 16384, 32768, 65536, 131072,
                      262144, 524288, 1048576, 2097152, 4194304};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < num_sizes; i++) {
        void *p = malloc(sizes[i]);
        if (p == NULL) {
            char msg[64];
            snprintf(msg, sizeof(msg), "malloc(%zu) returned NULL", sizes[i]);
            FAIL(msg);
            return;
        }
        memset(p, 0xCD, sizes[i]);
        free(p);
    }
    PASS();
}

static void test_malloc_zero(void) {
    TEST("malloc(0) behavior");
    void *p = malloc(0);
    /* malloc(0) may return NULL or a unique pointer - both are valid */
    if (p != NULL) {
        free(p);
    }
    PASS();
}

static void test_free_null(void) {
    TEST("free(NULL) is safe");
    free(NULL);  /* Should not crash */
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CALLOC TESTS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_calloc_zeroing(void) {
    TEST("calloc zeroes memory");
    size_t n = 1000;
    unsigned char *p = (unsigned char *)calloc(n, sizeof(unsigned char));
    if (p == NULL) { FAIL("calloc returned NULL"); return; }

    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0) {
            FAIL("calloc did not zero memory");
            free(p);
            return;
        }
    }
    free(p);
    PASS();
}

static void test_calloc_large(void) {
    TEST("calloc large (1000 x 4KB)");
    void *p = calloc(1000, 4096);
    if (p == NULL) { FAIL("calloc returned NULL"); return; }

    /* Verify zeroed */
    unsigned char *bytes = (unsigned char *)p;
    for (size_t i = 0; i < 1000 * 4096; i += 4096) {
        if (bytes[i] != 0) {
            FAIL("calloc did not zero large allocation");
            free(p);
            return;
        }
    }
    free(p);
    PASS();
}

static void test_calloc_overflow(void) {
    TEST("calloc overflow protection");
    /* This should return NULL due to overflow */
    void *p = calloc(SIZE_MAX, SIZE_MAX);
    if (p != NULL) {
        free(p);
        FAIL("calloc should return NULL on overflow");
        return;
    }
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * REALLOC TESTS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_realloc_grow(void) {
    TEST("realloc grow preserves data");
    char *p = (char *)malloc(100);
    if (p == NULL) { FAIL("initial malloc failed"); return; }

    memset(p, 'A', 100);

    char *p2 = (char *)realloc(p, 1000);
    if (p2 == NULL) { FAIL("realloc grow failed"); free(p); return; }

    for (int i = 0; i < 100; i++) {
        if (p2[i] != 'A') {
            FAIL("realloc did not preserve data");
            free(p2);
            return;
        }
    }
    free(p2);
    PASS();
}

static void test_realloc_shrink(void) {
    TEST("realloc shrink preserves data");
    char *p = (char *)malloc(1000);
    if (p == NULL) { FAIL("initial malloc failed"); return; }

    memset(p, 'B', 1000);

    char *p2 = (char *)realloc(p, 100);
    if (p2 == NULL) { FAIL("realloc shrink failed"); free(p); return; }

    for (int i = 0; i < 100; i++) {
        if (p2[i] != 'B') {
            FAIL("realloc shrink did not preserve data");
            free(p2);
            return;
        }
    }
    free(p2);
    PASS();
}

static void test_realloc_null(void) {
    TEST("realloc(NULL, size) == malloc(size)");
    void *p = realloc(NULL, 100);
    if (p == NULL) { FAIL("realloc(NULL, 100) returned NULL"); return; }
    memset(p, 0xEF, 100);
    free(p);
    PASS();
}

static void test_realloc_zero(void) {
    TEST("realloc(ptr, 0) behavior");
    void *p = malloc(100);
    if (p == NULL) { FAIL("malloc failed"); return; }
    void *p2 = realloc(p, 0);
    /* realloc(ptr, 0) may return NULL or a unique pointer */
    if (p2 != NULL) {
        free(p2);
    }
    PASS();
}

static void test_realloc_repeated(void) {
    TEST("repeated realloc (grow and shrink)");
    char *p = (char *)malloc(16);
    if (p == NULL) { FAIL("initial malloc failed"); return; }

    strcpy(p, "test");

    /* Grow */
    for (size_t size = 32; size <= 8192; size *= 2) {
        char *p2 = (char *)realloc(p, size);
        if (p2 == NULL) {
            FAIL("realloc grow failed");
            free(p);
            return;
        }
        p = p2;
        if (strcmp(p, "test") != 0) {
            FAIL("realloc corrupted data");
            free(p);
            return;
        }
    }

    /* Shrink */
    for (size_t size = 4096; size >= 16; size /= 2) {
        char *p2 = (char *)realloc(p, size);
        if (p2 == NULL) {
            FAIL("realloc shrink failed");
            free(p);
            return;
        }
        p = p2;
        if (strcmp(p, "test") != 0) {
            FAIL("realloc corrupted data");
            free(p);
            return;
        }
    }

    free(p);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ALIGNED ALLOCATION TESTS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_aligned_alloc(void) {
    TEST("aligned_alloc various alignments");
    size_t alignments[] = {16, 32, 64, 128, 256, 512, 1024, 4096};
    int num_alignments = sizeof(alignments) / sizeof(alignments[0]);

    for (int i = 0; i < num_alignments; i++) {
        size_t align = alignments[i];
        size_t size = align * 2;  /* Size must be multiple of alignment for C11 */

        void *p = aligned_alloc_impl(align, size);
        if (p == NULL) {
            char msg[64];
            snprintf(msg, sizeof(msg), "aligned_alloc(%zu, %zu) returned NULL", align, size);
            FAIL(msg);
            return;
        }

        if (((uintptr_t)p % align) != 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "aligned_alloc(%zu, %zu) not aligned", align, size);
            FAIL(msg);
            aligned_free_impl(p);
            return;
        }

        memset(p, 0xAA, size);
        aligned_free_impl(p);
    }
    PASS();
}

static void test_posix_memalign(void) {
    TEST("posix_memalign various alignments");
    size_t alignments[] = {sizeof(void*), 16, 32, 64, 128, 256, 512, 1024, 4096};
    int num_alignments = sizeof(alignments) / sizeof(alignments[0]);

    for (int i = 0; i < num_alignments; i++) {
        size_t align = alignments[i];
        void *p = NULL;

        int ret = posix_memalign_impl(&p, align, 1000);
        if (ret != 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "posix_memalign(%zu) returned %d", align, ret);
            FAIL(msg);
            return;
        }

        if (p == NULL) {
            FAIL("posix_memalign set pointer to NULL");
            return;
        }

        if (((uintptr_t)p % align) != 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "posix_memalign(%zu) not aligned", align);
            FAIL(msg);
            aligned_free_impl(p);
            return;
        }

        memset(p, 0xBB, 1000);
        aligned_free_impl(p);
    }
    PASS();
}

#if HAS_MEMALIGN
static void test_memalign(void) {
    TEST("memalign basic");
    void *p = memalign_impl(64, 1000);
    if (p == NULL) { FAIL("memalign returned NULL"); return; }

    if (((uintptr_t)p % 64) != 0) {
        FAIL("memalign(64) not 64-byte aligned");
        aligned_free_impl(p);
        return;
    }

    memset(p, 0xCC, 1000);
    aligned_free_impl(p);
    PASS();
}
#endif

#if HAS_VALLOC
static void test_valloc(void) {
    TEST("valloc (page-aligned)");
    long page_size = sysconf(_SC_PAGESIZE);

    void *p = valloc_impl(1000);
    if (p == NULL) { FAIL("valloc returned NULL"); return; }

    if (((uintptr_t)p % page_size) != 0) {
        FAIL("valloc not page-aligned");
        aligned_free_impl(p);
        return;
    }

    memset(p, 0xDD, 1000);
    aligned_free_impl(p);
    PASS();
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * SIZE AND STRING FUNCTION TESTS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_malloc_usable_size(void) {
    TEST("malloc_usable_size >= requested");
    void *p = malloc(100);
    if (p == NULL) { FAIL("malloc returned NULL"); return; }

    size_t usable = malloc_usable_size_impl(p);

    if (usable < 100) {
        char msg[64];
        snprintf(msg, sizeof(msg), "usable size %zu < requested 100", usable);
        FAIL(msg);
        free(p);
        return;
    }

    /* Only write to the requested size - usable size may include metadata */
    memset(p, 0xEE, 100);
    free(p);
    PASS();
}

static void test_strdup(void) {
    TEST("strdup");
    const char *original = "Hello, World! This is a test string for strdup.";
    char *copy = strdup(original);

    if (copy == NULL) { FAIL("strdup returned NULL"); return; }

    if (strcmp(copy, original) != 0) {
        FAIL("strdup did not copy string correctly");
        free(copy);
        return;
    }

    free(copy);
    PASS();
}

#if HAS_STRNDUP
static void test_strndup(void) {
    TEST("strndup");
    const char *original = "Hello, World!";
    char *copy = strndup(original, 5);

    if (copy == NULL) { FAIL("strndup returned NULL"); return; }

    if (strcmp(copy, "Hello") != 0) {
        FAIL("strndup did not copy correctly");
        free(copy);
        return;
    }

    free(copy);
    PASS();
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * STRESS TESTS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void test_many_small_allocs(void) {
    TEST("many small allocations (10000 x 64B)");
    const int N = 10000;
    void **ptrs = (void **)malloc(N * sizeof(void*));
    if (ptrs == NULL) { FAIL("could not allocate pointer array"); return; }

    for (int i = 0; i < N; i++) {
        ptrs[i] = malloc(64);
        if (ptrs[i] == NULL) {
            FAIL("malloc failed during mass allocation");
            for (int j = 0; j < i; j++) free(ptrs[j]);
            free(ptrs);
            return;
        }
        memset(ptrs[i], i & 0xFF, 64);
    }

    for (int i = 0; i < N; i++) {
        free(ptrs[i]);
    }
    free(ptrs);
    PASS();
}

static void test_alternating_alloc_free(void) {
    TEST("alternating alloc/free (10000 iterations)");
    for (int i = 0; i < 10000; i++) {
        void *p = malloc(64 + (i % 1024));
        if (p == NULL) { FAIL("malloc failed"); return; }
        memset(p, 0xAB, 64);
        free(p);
    }
    PASS();
}

static void test_random_sizes(void) {
    TEST("random-ish sizes (1000 allocations)");
    void *ptrs[100];
    int allocated = 0;

    for (int i = 0; i < 1000; i++) {
        /* Pseudo-random size based on iteration */
        size_t size = ((i * 7919) % 8192) + 1;

        if (allocated < 100 && (i % 3) != 0) {
            /* Allocate */
            ptrs[allocated] = malloc(size);
            if (ptrs[allocated] == NULL) {
                FAIL("malloc failed");
                for (int j = 0; j < allocated; j++) free(ptrs[j]);
                return;
            }
            memset(ptrs[allocated], 0xAB, size);
            allocated++;
        } else if (allocated > 0) {
            /* Free oldest */
            free(ptrs[0]);
            for (int j = 0; j < allocated - 1; j++) {
                ptrs[j] = ptrs[j + 1];
            }
            allocated--;
        }
    }

    /* Clean up remaining */
    for (int i = 0; i < allocated; i++) {
        free(ptrs[i]);
    }
    PASS();
}

static void test_alignment_default(void) {
    TEST("default alignment (pointer size minimum)");
    for (int i = 0; i < 100; i++) {
        void *p = malloc(1 + (i % 256));
        if (p == NULL) { FAIL("malloc failed"); return; }

        /* Standard requires alignment for any type */
        if (((uintptr_t)p % sizeof(void*)) != 0) {
            FAIL("pointer not aligned to pointer size");
            free(p);
            return;
        }
        free(p);
    }
    PASS();
}

static void test_large_alloc(void) {
    TEST("large allocation (16MB)");
    size_t size = 16 * 1024 * 1024;
    void *p = malloc(size);
    if (p == NULL) { FAIL("malloc(16MB) returned NULL"); return; }

    /* Touch the memory to ensure it's actually allocated */
    memset(p, 0xFF, size);
    free(p);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║              alloc8 Malloc API Test Suite                        ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");

    printf("Basic malloc/free:\n");
    test_malloc_free_basic();
    test_malloc_sizes();
    test_malloc_zero();
    test_free_null();

    printf("\nCalloc:\n");
    test_calloc_zeroing();
    test_calloc_large();
    test_calloc_overflow();

    printf("\nRealloc:\n");
    test_realloc_grow();
    test_realloc_shrink();
    test_realloc_null();
    test_realloc_zero();
    test_realloc_repeated();

    printf("\nAligned allocation:\n");
    test_aligned_alloc();
    test_posix_memalign();
#if HAS_MEMALIGN
    test_memalign();
#endif
#if HAS_VALLOC
    test_valloc();
#endif

    printf("\nSize and string functions:\n");
    test_malloc_usable_size();
    test_strdup();
#if HAS_STRNDUP
    test_strndup();
#endif

    printf("\nStress tests:\n");
    test_many_small_allocs();
    test_alternating_alloc_free();
    test_random_sizes();
    test_alignment_default();
    test_large_alloc();

    printf("\n╔══════════════════════════════════════════════════════════════════╗\n");
    if (tests_failed == 0) {
        printf("║  Results: %d/%d tests PASSED                                     ║\n", tests_passed, tests_run);
        printf("╚══════════════════════════════════════════════════════════════════╝\n");
        return 0;
    } else {
        printf("║  Results: %d/%d tests PASSED, %d FAILED                          ║\n", tests_passed, tests_run, tests_failed);
        printf("╚══════════════════════════════════════════════════════════════════╝\n");
        return 1;
    }
}

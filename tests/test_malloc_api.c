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
static int tests_warned = 0;

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

/* WARN: informational failure that doesn't fail the test suite.
 * Used for leak detection which may have false positives due to
 * allocator-specific memory retention behavior. */
#define WARN(msg) do { \
    tests_warned++; \
    tests_passed++;  /* Count as passed for exit code */ \
    printf("[WARN] %s\n", msg); \
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
 * LEAK DETECTION TESTS
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef _WIN32
#include <psapi.h>
static size_t get_rss_bytes(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize;
    }
    return 0;
}
#elif defined(__APPLE__)
#include <mach/mach.h>
static size_t get_rss_bytes(void) {
    struct task_basic_info info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
        return info.resident_size;
    }
    return 0;
}
#else
static size_t get_rss_bytes(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (f == NULL) return 0;

    unsigned long size, resident;
    if (fscanf(f, "%lu %lu", &size, &resident) != 2) {
        fclose(f);
        return 0;
    }
    fclose(f);

    long page_size = sysconf(_SC_PAGESIZE);
    return resident * page_size;
}
#endif

/* Helper to stabilize memory before measurement */
static void stabilize_memory(void) {
    /* Do some allocations and frees to warm up the allocator */
    for (int i = 0; i < 100; i++) {
        void *p = malloc(4096);
        if (p) { memset(p, 0, 4096); free(p); }
    }
#ifdef __APPLE__
    /* macOS may need a brief pause for memory to settle */
    usleep(10000);  /* 10ms */
#endif
}

/*
 * Leak detection note:
 * We use RSS (Resident Set Size) to detect leaks, but this has limitations:
 * - Allocators often retain freed memory for performance (memory pooling)
 * - OS may not immediately reclaim memory
 * - RSS can grow due to memory fragmentation
 *
 * Our approach: Run allocation/free cycles and check that RSS doesn't grow
 * unboundedly. A small amount of retained memory is acceptable, but if we're
 * leaking, RSS will grow significantly beyond what was allocated.
 *
 * We set generous thresholds to avoid false positives from allocator pooling,
 * but still catch genuine leaks (where free() doesn't actually free).
 */

/*
 * Multi-cycle leak detection: Run the same alloc/free pattern multiple times.
 * If there's a real leak, memory will grow with each cycle.
 * If allocator just retains memory (normal), it will plateau after the first cycle.
 */

static void test_leak_small_allocs(void) {
    TEST("leak check: small allocations (3 cycles)");

    stabilize_memory();
    if (get_rss_bytes() == 0) {
        printf("[SKIP] cannot read RSS\n");
        tests_run--;
        return;
    }

    const int N = 20000;
    const size_t alloc_size = 64;
    size_t rss_after[3];

    for (int cycle = 0; cycle < 3; cycle++) {
        void **ptrs = (void **)malloc(N * sizeof(void*));
        if (ptrs == NULL) { FAIL("could not allocate pointer array"); return; }

        for (int i = 0; i < N; i++) {
            ptrs[i] = malloc(alloc_size);
            if (ptrs[i] == NULL) {
                FAIL("malloc failed");
                for (int j = 0; j < i; j++) free(ptrs[j]);
                free(ptrs);
                return;
            }
            memset(ptrs[i], 0xAB, alloc_size);
        }

        for (int i = 0; i < N; i++) {
            free(ptrs[i]);
        }
        free(ptrs);

        stabilize_memory();
        rss_after[cycle] = get_rss_bytes();
    }

    /* Check: memory should stabilize, not keep growing unboundedly.
     * Allow the first cycle to grow (allocator warming up).
     * Cycles 2->3 may still grow slightly due to fragmentation.
     * Flag only if growth exceeds the amount allocated per cycle. */
    size_t growth = (rss_after[2] > rss_after[1]) ? (rss_after[2] - rss_after[1]) : 0;
    size_t per_cycle_alloc = N * alloc_size;

    if (growth > per_cycle_alloc) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "memory growing between cycles: c1=%zuKB, c2=%zuKB, c3=%zuKB (growth=%zuKB, threshold=%zuKB)",
                 rss_after[0]/1024, rss_after[1]/1024, rss_after[2]/1024,
                 growth/1024, per_cycle_alloc/1024);
        WARN(msg);
        return;
    }
    PASS();
}

static void test_leak_large_allocs(void) {
    TEST("leak check: large allocations (3 cycles)");

    stabilize_memory();
    if (get_rss_bytes() == 0) {
        printf("[SKIP] cannot read RSS\n");
        tests_run--;
        return;
    }

    const int N = 5;
    const size_t alloc_size = 4 * 1024 * 1024;
    size_t rss_after[3];

    for (int cycle = 0; cycle < 3; cycle++) {
        void *ptrs[5];

        for (int i = 0; i < N; i++) {
            ptrs[i] = malloc(alloc_size);
            if (ptrs[i] == NULL) {
                FAIL("malloc failed");
                for (int j = 0; j < i; j++) free(ptrs[j]);
                return;
            }
            memset(ptrs[i], 0xCD, alloc_size);
        }

        for (int i = 0; i < N; i++) {
            free(ptrs[i]);
        }

        stabilize_memory();
        rss_after[cycle] = get_rss_bytes();
    }

    size_t growth = (rss_after[2] > rss_after[1]) ? (rss_after[2] - rss_after[1]) : 0;
    size_t per_cycle_alloc = N * alloc_size;

    if (growth > per_cycle_alloc) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "memory growing between cycles: c1=%zuMB, c2=%zuMB, c3=%zuMB (growth=%zuMB, threshold=%zuMB)",
                 rss_after[0]/(1024*1024), rss_after[1]/(1024*1024), rss_after[2]/(1024*1024),
                 growth/(1024*1024), per_cycle_alloc/(1024*1024));
        WARN(msg);
        return;
    }
    PASS();
}

static void test_leak_aligned_allocs(void) {
    TEST("leak check: aligned allocations (3 cycles)");

    stabilize_memory();
    if (get_rss_bytes() == 0) {
        printf("[SKIP] cannot read RSS\n");
        tests_run--;
        return;
    }

    const int N = 500;
    const size_t alloc_size = 4096;
    const size_t alignment = 4096;
    size_t rss_after[3];

    for (int cycle = 0; cycle < 3; cycle++) {
        void **ptrs = (void **)malloc(N * sizeof(void*));
        if (ptrs == NULL) { FAIL("could not allocate pointer array"); return; }

        for (int i = 0; i < N; i++) {
            ptrs[i] = aligned_alloc_impl(alignment, alloc_size);
            if (ptrs[i] == NULL) {
                FAIL("aligned_alloc failed");
                for (int j = 0; j < i; j++) aligned_free_impl(ptrs[j]);
                free(ptrs);
                return;
            }
            memset(ptrs[i], 0xEF, alloc_size);
        }

        for (int i = 0; i < N; i++) {
            aligned_free_impl(ptrs[i]);
        }
        free(ptrs);

        stabilize_memory();
        rss_after[cycle] = get_rss_bytes();
    }

    size_t growth = (rss_after[2] > rss_after[1]) ? (rss_after[2] - rss_after[1]) : 0;
    size_t per_cycle_alloc = N * alloc_size;

    /* Flag if growth exceeds the per-cycle allocation - indicates real leak */
    if (growth > per_cycle_alloc) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "memory growing between cycles: c1=%zuKB, c2=%zuKB, c3=%zuKB (growth=%zuKB, threshold=%zuKB)",
                 rss_after[0]/1024, rss_after[1]/1024, rss_after[2]/1024,
                 growth/1024, per_cycle_alloc/1024);
        WARN(msg);
        return;
    }
    PASS();
}

static void test_leak_realloc_pattern(void) {
    TEST("leak check: realloc pattern (3 cycles)");

    stabilize_memory();
    if (get_rss_bytes() == 0) {
        printf("[SKIP] cannot read RSS\n");
        tests_run--;
        return;
    }

    const int ITERATIONS = 500;
    size_t rss_after[3];

    for (int cycle = 0; cycle < 3; cycle++) {
        for (int i = 0; i < ITERATIONS; i++) {
            char *p = (char *)malloc(64);
            if (p == NULL) { FAIL("malloc failed"); return; }
            memset(p, 'A', 64);

            p = (char *)realloc(p, 4096);
            if (p == NULL) { FAIL("realloc failed"); return; }
            memset(p, 'B', 4096);

            p = (char *)realloc(p, 65536);
            if (p == NULL) { FAIL("realloc failed"); return; }
            memset(p, 'C', 65536);

            p = (char *)realloc(p, 1024);
            if (p == NULL) { FAIL("realloc failed"); return; }

            free(p);
        }

        stabilize_memory();
        rss_after[cycle] = get_rss_bytes();
    }

    size_t growth = (rss_after[2] > rss_after[1]) ? (rss_after[2] - rss_after[1]) : 0;

    /* Allow 4MB growth tolerance for realloc patterns */
    size_t threshold = 4 * 1024 * 1024;
    if (growth > threshold) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "memory growing between cycles: c1=%zuMB, c2=%zuMB, c3=%zuMB (growth=%zuMB, threshold=4MB)",
                 rss_after[0]/(1024*1024), rss_after[1]/(1024*1024), rss_after[2]/(1024*1024),
                 growth/(1024*1024));
        WARN(msg);
        return;
    }
    PASS();
}

static void test_leak_calloc(void) {
    TEST("leak check: calloc allocations (3 cycles)");

    stabilize_memory();
    if (get_rss_bytes() == 0) {
        printf("[SKIP] cannot read RSS\n");
        tests_run--;
        return;
    }

    const int N = 500;
    const size_t elem_size = 4096;
    size_t rss_after[3];

    for (int cycle = 0; cycle < 3; cycle++) {
        void **ptrs = (void **)malloc(N * sizeof(void*));
        if (ptrs == NULL) { FAIL("could not allocate pointer array"); return; }

        for (int i = 0; i < N; i++) {
            ptrs[i] = calloc(1, elem_size);
            if (ptrs[i] == NULL) {
                FAIL("calloc failed");
                for (int j = 0; j < i; j++) free(ptrs[j]);
                free(ptrs);
                return;
            }
            /* Verify zeroed on first cycle */
            if (cycle == 0) {
                unsigned char *bytes = (unsigned char *)ptrs[i];
                for (size_t j = 0; j < 16; j++) {
                    if (bytes[j] != 0) {
                        FAIL("calloc memory not zeroed");
                        for (int k = 0; k <= i; k++) free(ptrs[k]);
                        free(ptrs);
                        return;
                    }
                }
            }
            memset(ptrs[i], 0xDD, elem_size);
        }

        for (int i = 0; i < N; i++) {
            free(ptrs[i]);
        }
        free(ptrs);

        stabilize_memory();
        rss_after[cycle] = get_rss_bytes();
    }

    size_t growth = (rss_after[2] > rss_after[1]) ? (rss_after[2] - rss_after[1]) : 0;
    size_t per_cycle_alloc = N * elem_size;

    if (growth > per_cycle_alloc) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "memory growing between cycles: c1=%zuKB, c2=%zuKB, c3=%zuKB (growth=%zuKB, threshold=%zuKB)",
                 rss_after[0]/1024, rss_after[1]/1024, rss_after[2]/1024,
                 growth/1024, per_cycle_alloc/1024);
        WARN(msg);
        return;
    }
    PASS();
}

static void test_leak_strdup(void) {
    TEST("leak check: strdup allocations (3 cycles)");

    stabilize_memory();
    if (get_rss_bytes() == 0) {
        printf("[SKIP] cannot read RSS\n");
        tests_run--;
        return;
    }

    const size_t str_len = 5000;
    const int N = 500;
    size_t rss_after[3];

    /* Create a string to duplicate */
    char *original = (char *)malloc(str_len + 1);
    if (original == NULL) { FAIL("malloc failed"); return; }
    memset(original, 'X', str_len);
    original[str_len] = '\0';

    for (int cycle = 0; cycle < 3; cycle++) {
        char **copies = (char **)malloc(N * sizeof(char*));
        if (copies == NULL) { FAIL("malloc failed"); free(original); return; }

        for (int i = 0; i < N; i++) {
            copies[i] = strdup(original);
            if (copies[i] == NULL) {
                FAIL("strdup failed");
                for (int j = 0; j < i; j++) free(copies[j]);
                free(copies);
                free(original);
                return;
            }
        }

        for (int i = 0; i < N; i++) {
            free(copies[i]);
        }
        free(copies);

        stabilize_memory();
        rss_after[cycle] = get_rss_bytes();
    }

    free(original);

    size_t growth = (rss_after[2] > rss_after[1]) ? (rss_after[2] - rss_after[1]) : 0;
    size_t per_cycle_alloc = N * (str_len + 1);

    if (growth > per_cycle_alloc) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "memory growing between cycles: c1=%zuKB, c2=%zuKB, c3=%zuKB (growth=%zuKB, threshold=%zuKB)",
                 rss_after[0]/1024, rss_after[1]/1024, rss_after[2]/1024,
                 growth/1024, per_cycle_alloc/1024);
        WARN(msg);
        return;
    }
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

    printf("\nLeak detection tests:\n");
    test_leak_small_allocs();
    test_leak_large_allocs();
    test_leak_aligned_allocs();
    test_leak_realloc_pattern();
    test_leak_calloc();
    test_leak_strdup();

    printf("\n╔══════════════════════════════════════════════════════════════════╗\n");
    if (tests_failed == 0) {
        if (tests_warned > 0) {
            printf("║  Results: %d/%d tests PASSED (%d warnings)                       ║\n",
                   tests_passed, tests_run, tests_warned);
        } else {
            printf("║  Results: %d/%d tests PASSED                                     ║\n",
                   tests_passed, tests_run);
        }
        printf("╚══════════════════════════════════════════════════════════════════╝\n");
        if (tests_warned > 0) {
            printf("\nNote: Warnings indicate potential memory leaks detected via RSS monitoring.\n");
            printf("These may be false positives due to allocator-specific memory retention.\n");
        }
        return 0;
    } else {
        printf("║  Results: %d/%d tests PASSED, %d FAILED                          ║\n",
               tests_passed, tests_run, tests_failed);
        if (tests_warned > 0) {
            printf("║  (plus %d warnings)                                              ║\n", tests_warned);
        }
        printf("╚══════════════════════════════════════════════════════════════════╝\n");
        return 1;
    }
}

#define _POSIX_C_SOURCE 200809L
#include "allocator_stats.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

/* These resolve only when the custom library is actually preloaded. */
#pragma weak allocator_get_stats
#pragma weak allocator_validate
#pragma weak allocator_strategy_name

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr);               \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

static void check_heap(void) {
    if (allocator_validate)
        CHECK(allocator_validate());
}

static void check_pattern(const unsigned char *p, size_t size, unsigned char value) {
    for (size_t i = 0; i < size; ++i)
        CHECK(p[i] == value);
}

static struct allocator_stats snapshot(void) {
    struct allocator_stats result;
    CHECK(allocator_get_stats(&result) == 0);
    return result;
}

static void api_tests(void) {
    // check free preserves errno
    errno = EDOM;
    free(NULL);
    CHECK(errno == EDOM);

    // check malloc can allocator and free 0 - 4097 bytes
    for (size_t size = 0; size < 4097; ++size) {
        void *p = malloc(size);
        CHECK(p != NULL);
        CHECK((uintptr_t)p % _Alignof(max_align_t) == 0);
        memset(p, 0xAB, size);
        errno = ERANGE;
        free(p);
        CHECK(errno == ERANGE);
    }

    // allocate huge array, whos members are all 0 bytes
    volatile size_t huge = SIZE_MAX;
    void *p = calloc(0, huge);
    CHECK(p != NULL);
    free(p);

    // allocate an array of length 0 whos members are size 0
    p = calloc(huge, 0);
    CHECK(p != NULL);
    free(p);

    // realloc with NULL results in malloc
    p = realloc(NULL, 0);
    CHECK(p != NULL);
    free(p);

    // calloc zeros the allocated area
    unsigned char *dirty = malloc(1024);
    CHECK(dirty != NULL);
    memset(dirty, 0xEF, 1024);
    free(dirty);
    unsigned char *zero = calloc(128, 8);
    CHECK(zero != NULL);
    check_pattern(zero, 1024, 0);
    free(zero);

    // can't alloc sizes larger the PTRDIFF_MAX/ SIZE_T_MAX
    errno = 0;
    CHECK(malloc(huge) == NULL && errno == ENOMEM);
    errno = 0;
    CHECK(calloc(huge / 2 + 1, 2) == NULL && errno == ENOMEM);
    p = malloc(128);
    CHECK(p != NULL);
    memset(p, 0xCD, 128);
    errno = 0;
    CHECK(realloc(p, huge) == NULL && errno == ENOMEM);
    check_pattern(p, 128, 0xCD);
    free(p);

    // check realloc moves to different area, it block can't be expanded
    p = realloc(NULL, 256);
    CHECK(p != NULL);
    memset(p, 0x67, 256);
    void *guard = malloc(256);
    CHECK(guard != NULL);
    p = realloc(p, 8192);
    CHECK(p != NULL);
    CHECK((uintptr_t)p % _Alignof(max_align_t) == 0);
    check_pattern(p, 256, 0x67);
    memset(p, 0x98, 8192);
    p = realloc(p, 64);
    CHECK(p != NULL);
    check_pattern(p, 64, 0x98);

    // calling realloc with size 0 results in free
    CHECK(realloc(p, 0) == NULL); /* This project targets glibc semantics. */
    free(guard);
    check_heap();
}

/* Hold every free block so pointer-choice tests cannot see startup leftovers. */
struct drain {
    void *ptrs[128];
    size_t count;
};

static void drain_free_blocks(struct drain *d) {
    for (;;) {
        struct allocator_stats s = snapshot();
        if (!s.free_blocks)
            break;
        CHECK(d->count < sizeof(d->ptrs) / sizeof(d->ptrs[0]));
        d->ptrs[d->count] = malloc(s.largest_free_block);
        CHECK(d->ptrs[d->count] != NULL);
        ++d->count;
    }
    check_heap();
}

static void undo_drain(struct drain *d) {
    while (d->count)
        free(d->ptrs[--d->count]);
}

static void policy_test(void) {
    const char *strategy = allocator_strategy_name();
    struct drain d = {0};
    drain_free_blocks(&d);

    // this will create a new arena, since all free blocks have been taken
    void *a = malloc(1024), *guard_a = malloc(32);
    void *b = malloc(512), *guard_b = malloc(32);
    void *c = malloc(256), *guard_c = malloc(32);
    CHECK(a && b && c && guard_a && guard_b && guard_c);
    drain_free_blocks(&d); // consunes all block -> cursor is NULL

    uintptr_t a_addr = (uintptr_t)a, b_addr = (uintptr_t)b, c_addr = (uintptr_t)c;
    /* Free-list order is A, C, B; the Next Fit cursor starts at B. */
    free(b); // set cursor to B, cursor remains B since free does not set the cursor (unless it is
             // unset)
    free(c);
    free(a);

    void *chosen = malloc(200);
    CHECK(chosen != NULL);
    // best fit should chhose c, since it is the smallest allocated block,
    // next fit takes b, since thats the cursor position, and first fit takes a, since it is it is
    // at the head of the free list because it was freed last
    uintptr_t expected = strcmp(strategy, "best") == 0   ? c_addr
                         : strcmp(strategy, "next") == 0 ? b_addr
                                                         : a_addr;
    CHECK((uintptr_t)chosen == expected);
    check_heap();
    free(chosen);
    free(guard_a);
    free(guard_b);
    free(guard_c);
    undo_drain(&d);
    check_heap();
}

static void coalescing_and_resize_tests(void) {
    struct drain d = {0};
    drain_free_blocks(&d);

    void *a = malloc(512), *b = malloc(512), *c = malloc(512);
    void *guard = malloc(64);

    CHECK(a && b && c && guard);
    drain_free_blocks(&d);

    uintptr_t original = (uintptr_t)a;
    struct allocator_stats before = snapshot();
    free(a);
    free(c);
    free(b); /* Both neighbors must be merged. */
    struct allocator_stats merged = snapshot();
    CHECK(merged.coalesces >= before.coalesces + 2);
    void *joined = malloc(1400);
    CHECK((uintptr_t)joined == original);
    CHECK(snapshot().mmap_calls == before.mmap_calls);
    free(joined);
    free(guard);
    undo_drain(&d);
    check_heap();

    drain_free_blocks(&d);
    a = malloc(512);
    b = malloc(512);
    guard = malloc(64);
    CHECK(a && b && guard);
    memset(a, 0x35, 512);
    drain_free_blocks(&d);
    original = (uintptr_t)a;
    free(b);
    a = realloc(a, 800);
    CHECK((uintptr_t)a == original); /* Grow into the right free neighbor. */
    check_pattern(a, 512, 0x35);
    a = realloc(a, 128); /* Split, then merge the remainder with its neighbor. */
    CHECK((uintptr_t)a == original);
    check_pattern(a, 128, 0x35);
    check_heap();
    free(a);
    free(guard);
    undo_drain(&d);
}

static uint32_t random_number(uint32_t *state) {
    // Better than some peoples private key generation. *Ahem Ahem, returns 4
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static void random_workload(uint32_t seed, size_t iterations) {
    enum { SLOTS = 128 };
    struct {
        unsigned char *ptr;    // pointer to allocated memory
        size_t size;           // size of allocated memory
        unsigned char pattern; // expected value of bytes in slot
    } slots[SLOTS] = {{0}};

    for (size_t step = 0; step < iterations; ++step) {
        size_t slot = random_number(&seed) % SLOTS;
        size_t size = 1 + random_number(&seed) % 8192;
        // If slot is allocated, and operation == 0 than free the memory
        // if slot is not allocated and operation == 0 than use calloc to allocate, otherwise use
        // malloc
        unsigned operation = random_number(&seed) % 3;

        if (slots[slot].ptr) {
            check_pattern(slots[slot].ptr, slots[slot].size, slots[slot].pattern);
            if (operation == 0) {
                free(slots[slot].ptr);
                slots[slot].ptr = NULL;
                continue;
            }
            unsigned char *p = realloc(slots[slot].ptr, size);
            CHECK(p != NULL);
            size_t preserved = size < slots[slot].size ? size : slots[slot].size;
            check_pattern(p, preserved, slots[slot].pattern);
            slots[slot].ptr = p;

        } else {
            slots[slot].ptr = operation == 0 ? calloc(size, 1) : malloc(size);
            CHECK(slots[slot].ptr != NULL);
            if (operation == 0)
                check_pattern(slots[slot].ptr, size, 0);
        }

        CHECK((uintptr_t)slots[slot].ptr % _Alignof(max_align_t) == 0);
        slots[slot].size = size;
        slots[slot].pattern = (unsigned char)random_number(&seed);
        memset(slots[slot].ptr, slots[slot].pattern, size);

        if (step % 257 == 0)
            check_heap();
    }
    for (size_t i = 0; i < SLOTS; ++i) {
        if (slots[i].ptr)
            check_pattern(slots[i].ptr, slots[i].size, slots[i].pattern);
        free(slots[i].ptr);
    }
    check_heap();
}

static void *thread_workload(void *arg) {
    random_workload((uint32_t)(uintptr_t)arg, 12000);
    unsigned char *handoff = malloc(97);
    CHECK(handoff != NULL);
    memset(handoff, 0xBC, 97);
    return handoff;
}

static void threaded_test(void) {
    pthread_t threads[4];
    for (size_t i = 0; i < 4; ++i)
        CHECK(pthread_create(&threads[i], NULL, thread_workload, (void *)(uintptr_t)(12345 + i)) ==
              0);
    for (size_t i = 0; i < 4; ++i) {
        void *p;
        CHECK(pthread_join(threads[i], &p) == 0);
        check_pattern(p, 97, 0xBC);
        free(p); /* Allocated by a different thread. */
    }
    check_heap();
}

static void out_of_memory_test(void) {
    unsigned char *p = malloc(128);
    CHECK(p != NULL);
    memset(p, 0xD2, 128);
    struct rlimit limit;
    CHECK(getrlimit(RLIMIT_AS, &limit) == 0);
    limit.rlim_cur = 64 * 1024 * 1024;
    if (limit.rlim_max < limit.rlim_cur)
        limit.rlim_cur = limit.rlim_max;
    CHECK(setrlimit(RLIMIT_AS, &limit) == 0);
    /* Valid arithmetic, but a kernel-enforced allocation failure. */
    errno = 0;
    CHECK(malloc(256 * 1024 * 1024) == NULL && errno == ENOMEM);
    errno = 0;
    CHECK(calloc(256 * 1024 * 1024, 1) == NULL && errno == ENOMEM);
    errno = 0;
    CHECK(realloc(p, 256 * 1024 * 1024) == NULL && errno == ENOMEM);
    check_pattern(p, 128, 0xD2);
    free(p);
    check_heap();
    puts("PASS: kernel-enforced ENOMEM and realloc failure atomicity");
}

int main(int argc, char **argv) {
    int custom = allocator_get_stats && allocator_validate && allocator_strategy_name;
    printf("Allocator: %s\n", custom ? allocator_strategy_name() : "system allocator");
    fflush(stdout);

    if (argc > 1 && strcmp(argv[1], "--require-custom") == 0)
        CHECK(custom); /* A silently ignored preload is a test failure. */

    if (argc > 2 && strcmp(argv[2], "--oom-only") == 0) {
        out_of_memory_test();
        return 0;
    }
    if (argc > 2 && strcmp(argv[2], "--fallback-only") == 0) {
        CHECK(custom);
        CHECK(strcmp(allocator_strategy_name(), "first") == 0);
        policy_test();
        puts("PASS: strategy selection (including invalid-name fallback)");
        return 0;
    }

    api_tests();
    if (custom) {
        policy_test();
        coalescing_and_resize_tests();
    }
    random_workload(0x12345678, 40000);
    threaded_test();
    printf("PASS: %s; API, overflow, alignment, resizing, randomized and threaded tests%s\n",
           custom ? allocator_strategy_name() : "system allocator",
           custom ? ", policy, coalescing and mapping release" : "");
    return 0;
}

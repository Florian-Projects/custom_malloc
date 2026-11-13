#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "allocator.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

// default arena size of 64 KiB
#define ARENA_BYTES ((size_t)64 * 1024)
#define ALIGNMENT _Alignof(max_align_t)

typedef union block block;
typedef union arena arena;

union block {
    struct {
        size_t capacity,
            requested; /* capacity is the actual amount of bytes available in a block. requested is
                          the amount of bytes requested from malloc. capacity may be larger than
                          requested because capacity is rounded to the next alignment */
        block *prev, *next;           /* Neighbors within the same arena. */
        block *free_prev, *free_next; /* Unsorted, doubly linked free list. */
        arena *arena;
        bool free;
    } h;
    max_align_t alignment; /* used for aligning the union */
};

union arena {
    struct {
        arena *prev, *next;
        size_t length;
    } h;
    max_align_t alignment;
};

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
enum strategy { FIRST, BEST, NEXT };
static enum strategy policy = FIRST;
static arena *arenas, *spare = NULL;
static block *free_head, *cursor = NULL;

// arena size is a multiple of page size. page size is fetched from kernel
static size_t page_size, arena_size;
static bool initialized = false;

static void initialize(void) {
    if (initialized)
        return;

    const char *name = getenv("ALLOC_STRATEGY");
    policy = name && strcmp(name, "best") == 0   ? BEST
             : name && strcmp(name, "next") == 0 ? NEXT
                                                 : FIRST;

    long queried = sysconf(_SC_PAGESIZE);
    page_size = queried > 0 ? (size_t)queried : 4096;

    // round Arena bytes to a multiple of page size
    arena_size = ((ARENA_BYTES + page_size - 1) / page_size) * page_size;
    initialized = true;
}

static bool aligned_size(size_t requested, size_t *result) {
    size_t n = requested ? requested : 1; // block has minimum capacity of 1

    // ensure rounding up to ALIGNMENT does not result in a a size thats larger than the result of
    // pointer arithmatic
    if (n > (size_t)PTRDIFF_MAX - (ALIGNMENT - 1)) {
        errno = ENOMEM;
        return false;
    }

    // round up to the nearest alignment
    *result = ((n + ALIGNMENT - 1) / ALIGNMENT) * ALIGNMENT;
    return true;
}

static void add_free(block *b) {
    // add free block to head of free list
    b->h.free = true;
    b->h.free_prev = NULL;
    b->h.free_next = free_head;
    if (free_head)
        free_head->h.free_prev = b;

    free_head = b;
    if (!cursor)
        cursor = b;
}

static void remove_free(block *b) {
    if (b->h.free_prev)
        b->h.free_prev->h.free_next = b->h.free_next;
    else
        free_head = b->h.free_next;

    if (b->h.free_next)
        b->h.free_next->h.free_prev = b->h.free_prev;

    if (cursor == b)
        cursor = b->h.free_next ? b->h.free_next : free_head;

    b->h.free_prev = b->h.free_next = NULL;
    b->h.free = false;
}

static block *find_free_block(size_t needed) {
    if (policy == NEXT) {
        block *start = cursor ? cursor : free_head;
        if (!start)
            return NULL;

        block *b = start;
        do {
            if (b->h.capacity >= needed) {
                cursor = b;
                return b;
            }
            b = b->h.free_next ? b->h.free_next : free_head;
        } while (b != start);
        return NULL;
    }
    block *best = NULL;
    for (block *b = free_head; b; b = b->h.free_next) {
        if (b->h.capacity < needed)
            continue;
        if (policy == FIRST || b->h.capacity == needed)
            return b;
        if (!best || b->h.capacity < best->h.capacity)
            best = b;
    }
    return best;
}

static void merge_right(block *left, block *right) {
    // absorb the already freed right block into the left
    left->h.capacity += sizeof(block) + right->h.capacity;
    left->h.next = right->h.next;
    if (right->h.next)
        left->h.next->h.prev = left;
}

static void split(block *b, size_t needed) {
    // Split the block into 2 blocks.
    // The first containing the needed capacity, and the second the rest until the next
    // non free block
    if (b->h.capacity - needed < sizeof(block) + ALIGNMENT)
        // block too small to split
        return;

    block *rest = (block *)((unsigned char *)(b + 1) + needed);

    rest->h.capacity = b->h.capacity - needed - sizeof(block);
    rest->h.requested = 0;
    rest->h.arena = b->h.arena;
    rest->h.prev = b;
    rest->h.next = b->h.next;
    if (rest->h.next)
        rest->h.next->h.prev = rest;
    b->h.next = rest;
    b->h.capacity = needed;

    // we just created a new free block
    // if the next block is also free, they need to be merged into one
    if (rest->h.next && rest->h.next->h.free) {
        block *right = rest->h.next;
        remove_free(right);
        merge_right(rest, right);
    }

    add_free(rest);
    cursor = rest;
}

static block *map_arena(size_t needed) {
    // The arena needs to have space for both the requested size and the arena and block meta data
    size_t overhead = sizeof(arena) + sizeof(block);
    if (needed > (size_t)PTRDIFF_MAX - overhead - (page_size - 1)) {
        errno = ENOMEM;
        return NULL;
    }

    // arena is at least arena_size large and a multiple of page_size
    size_t length = needed + overhead;
    if (length < arena_size)
        length = arena_size;
    length = ((length + page_size - 1) / page_size) * page_size;

    arena *a = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED)
        return NULL;

    a->h.length = length;

    a->h.prev = NULL;
    a->h.next = arenas;
    if (arenas)
        arenas->h.prev = a;
    arenas = a;

    block *b = (block *)(a + 1);
    b->h.capacity = length - overhead;
    b->h.requested = 0;
    b->h.prev = b->h.next = NULL;
    b->h.arena = a;
    add_free(b);
    return b;
}

static void *allocate(size_t requested) {
    size_t needed;
    if (!aligned_size(requested, &needed))
        return NULL;

    block *b = find_free_block(needed);

    // The free-list search includes the spare. Map a new arena only if no block fits.
    if (!b && !(b = map_arena(needed)))
        return NULL;

    remove_free(b);
    b->h.free = false;
    b->h.requested = requested;

    if (b->h.arena == spare) {
        spare = NULL;
    }

    split(b, needed);
    return b + 1;
}

static void release(void *ptr) {
    // release address, merging free adjacent blocks in the process
    if (!ptr)
        return;

    block *b = (block *)ptr - 1;
    b->h.requested = 0;
    b->h.free = true;

    if (b->h.prev && b->h.prev->h.free) {
        block *left = b->h.prev;
        remove_free(left);
        merge_right(left, b);
        b = left;
    }
    if (b->h.next && b->h.next->h.free) {
        block *right = b->h.next;
        remove_free(right);
        merge_right(b, right);
    }

    add_free(b);
    if (b->h.prev || b->h.next)
        return;

    arena *a = b->h.arena;

    // keep at most one spare normal size arena
    if (!spare && a->h.length == arena_size) {
        spare = a;
        return;
    }

    size_t length = a->h.length;
    arena *prev = a->h.prev, *next = a->h.next;
    remove_free(b);

    if (munmap(a, length) != 0) {
        add_free(b); // the arena remains usuable on failure
        return;
    }

    if (prev)
        prev->h.next = next;
    else
        arenas = next;
    if (next)
        next->h.prev = prev;
}

EXPORT void *malloc(size_t size) {
    pthread_mutex_lock(&mutex);
    initialize();
    void *result = allocate(size);
    pthread_mutex_unlock(&mutex);
    return result;
}

EXPORT void free(void *ptr) {
    int saved_errno = errno;
    pthread_mutex_lock(&mutex);
    release(ptr);
    pthread_mutex_unlock(&mutex);
    errno = saved_errno;
}

EXPORT void *realloc(void *ptr, size_t size) {
    int saved_errno = errno;
    pthread_mutex_lock(&mutex);
    initialize();
    void *result = NULL;

    if (!ptr) {
        result = allocate(size);
    } else if (!size) {
        // glibc convetion: realloc(p, 0) results in free
        release(ptr);
        errno = saved_errno;
    } else {
        size_t needed;
        if (aligned_size(size, &needed)) {
            block *b = (block *)ptr - 1;

            // if the current block is too small to fullfill the reallocation,
            // check if the next block is free and if it adds enough capacity
            // if it does add it to the current block
            if (needed > b->h.capacity && b->h.next && b->h.next->h.free &&
                needed <= b->h.capacity + sizeof(block) + b->h.next->h.capacity) {
                block *right = b->h.next;
                remove_free(right);
                merge_right(b, right);
            }

            // if the ccurent block is larger than what we need, we split the rest of
            if (needed <= b->h.capacity) {
                split(b, needed);
                b->h.requested = size;
                result = ptr;
            } else {
                // the requested new size does not fit into the current/ current + next block.
                // We need to find a completely new block
                result = allocate(size);
                if (result) {
                    memcpy(result, ptr, b->h.requested);
                    release(ptr);
                }
            }
        }
    }

    pthread_mutex_unlock(&mutex);
    return result;
}

EXPORT void *calloc(size_t count, size_t size) {
    pthread_mutex_lock(&mutex);
    initialize();
    void *result = NULL;
    if (size && count > SIZE_MAX / size) {
        errno = ENOMEM;

    } else {
        size_t bytes = count * size;
        result = allocate(bytes);
        if (result)
            memset(result, 0, bytes);
    }
    pthread_mutex_unlock(&mutex);
    return result;
}

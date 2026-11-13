#ifndef ALLOCATOR_STATS_H
#define ALLOCATOR_STATS_H

#include <stddef.h>

/* Current values unless named peak_* or described as cumulative below. */
struct allocator_stats {
    size_t mapped_bytes, peak_mapped_bytes, arenas;
    size_t live_blocks, requested_bytes, capacity_bytes;
    /* Sampled from the free list when requesting a snapshot. */
    size_t free_blocks, free_bytes, largest_free_block;
    /* Cumulative public calls, including failed requests and free(NULL). */
    size_t malloc_calls, free_calls, calloc_calls, realloc_calls;
    /* Cumulative search invocations, inspected entries, splits, and pairwise merges. */
    size_t searches, search_steps, splits, coalesces;
    /* Cumulative system-call attempts, including failures. */
    size_t mmap_calls, munmap_calls;
    /* Mean free payload capacity in bytes, sampled on demand; 0.0 if none. */
    double average_free_block_size;
};

/* Diagnostics take the allocator mutex and never allocate memory. */
/* Return 0 on success, or -1 when out is NULL. */
int allocator_get_stats(struct allocator_stats *out);
/* Return 1 when the heap and accounting are consistent, otherwise 0. */
int allocator_validate(void);
const char *allocator_strategy_name(void);

#endif

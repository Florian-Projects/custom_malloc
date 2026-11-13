# Disclaimer
This README has been written by chatgpt. Its contents have been validated but not modified

# Allocator overview

This allocator obtains memory from the operating system in **arenas**, divides
them into blocks, and reuses free blocks for later allocations.

## Data structures

- **Arena list:** a doubly linked list of all currently mapped arenas, including
  the spare. Each arena records its neighbors and total mapping length.
- **Physical block chains:** each arena has its own doubly linked chain of blocks
  in memory order. These links identify adjacent blocks for merging.
- **Free list:** one global doubly linked list contains free blocks from all
  arenas. Blocks are inserted at its head, so its order can differ from physical
  order.
- **Next Fit cursor:** identifies where the next search starts. Removing a free
  block repairs the cursor if necessary; splitting sets it to the remainder.

`ALLOC_STRATEGY` selects `best` or `next`; other values, including an unset
variable, select First Fit. First Fit takes the first suitable free-list entry,
Best Fit takes the smallest suitable entry, and Next Fit searches from its cursor,
wrapping around once.

## Metadata layout

Each mapping starts with an arena header, followed by block headers and payloads:

```text
[arena header][block header][payload][block header][payload] ...
```

An arena header stores `prev`, `next`, and `length`. Each block header stores:

| Field                    | Meaning                                                                       |
| ------------------------ | ----------------------------------------------------------------------------- |
| `capacity`               | Payload bytes physically available in this block; excludes its header.        |
| `requested`              | Bytes requested by the caller; may be smaller than capacity. Due to alignment |
| `prev`, `next`           | Physical neighbors within the same arena.                                     |
| `free_prev`, `free_next` | Neighbors in the global free list.                                            |
| `arena`                  | Pointer to the owning arena.                                                  |
| `free`                   | Whether the block is available for allocation.                                |

The headers are unions containing both their metadata struct and a `max_align_t`
member. That member ensures suitable header alignment; it is not used to store
data. Header sizes and payload capacities are multiples of
`A = _Alignof(max_align_t)`, keeping subsequent headers and payloads aligned.

For a block pointer `b`, `b + 1` points to the payload's beginning. From the exact
payload pointer returned to the caller, `(block *)ptr - 1` recovers the header.

## Splitting and merging

Allocation reserves at least one byte and rounds the storage requirement up to
a multiple of `A`. A block is split only when the leftover space can hold another
block header plus at least `A` payload bytes. Otherwise, the allocation keeps the
whole block. Any free right neighbor is merged into the remainder before the
remainder is added to the free list. Shrinking an allocation uses the same
splitting logic.

Freeing immediately merges any free physical neighbors on either side. Merging
preserves the left header and absorbs the right header and payload into the left
block's capacity. It updates links without moving payload bytes. Participating
free-list entries are removed before merging, and the resulting free block is
inserted once. Blocks from different arenas are never merged.

## Mapping and unmapping

When no free block fits, `mmap` creates a readable, writable, private anonymous
mapping. The normal arena size is 64 KiB rounded up to whole system pages. Larger
requests receive a larger mapping that includes both headers and the payload,
also rounded to whole pages. The page size comes from `sysconf(_SC_PAGESIZE)`,
with a 4096-byte fallback. Size arithmetic is checked before mapping.

After freeing and merging, a single free block with no physical neighbors means
the entire arena is empty. The allocator retains one normal-sized empty arena
as `spare`; other empty arenas are passed to `munmap`. The spare stays in the
arena list with its block in the free list. Allocating from it clears the spare
marker.

Before unmapping, the allocator saves the arena's length and list neighbors and
removes its free block. Success unlinks the arena using the saved pointers.
Failure restores its free-list entry, so the mapping remains reusable; this can
leave additional empty arenas mapped.

## Concurrency

One global mutex protects initialization, arena and free lists, block metadata,
and the spare and cursor pointers. Public allocation functions acquire it;
internal helpers run while it is held.

## Statistics and diagnostics

Build `allocat_with_stats.c` instead of `allocator.c` to enable statistics and
diagnostics. The two files are independent versions exporting the same allocation
functions; compile only one into a given allocator library.

Include `allocator_stats.h` to use the instrumented version's diagnostics:

- `allocator_get_stats(&stats)` copies a consistent snapshot into a
  `struct allocator_stats`. It returns `0` on success or `-1` for a null pointer.
- `allocator_validate()` returns `1` when arena/block links, alignment, free-list
  state, and live-memory accounting are consistent, or `0` otherwise.
- `allocator_strategy_name()` returns `"first"`, `"best"`, or `"next"`.

Snapshots report current and peak mapped bytes, arena and live-block counts,
requested bytes, allocated payload capacity, and free-space distribution.
`capacity_bytes - requested_bytes` measures slack inside live allocations;
headers are excluded from both values.

Call counters are cumulative and include failures and `free(NULL)`. Internal
allocation and release during `realloc` do not increment `malloc_calls` or
`free_calls`. Search, split, merge, and mapping-attempt counters are also
cumulative. Mapped-byte and arena totals change only after successful system
calls. Free-block statistics are calculated from the free list for each snapshot.
`average_free_block_size` is the mean free payload capacity in bytes, including
the spare's free block and excluding headers. It is a `double` to preserve
fractional averages, or `0.0` when no free blocks exist. It is calculated in
`allocator_get_stats` from the same snapshot's free bytes and block count, so
validation does not need to run first. Rebuild consumers with the updated
`allocator_stats.h` when using the extended statistics structure.
All diagnostics use the existing mutex and perform no allocations.

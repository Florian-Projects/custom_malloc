The first 2 section deal with implementation and benchmark details.
Evaluation of the allocator starts here: Evaluation

This document was written in obsidian and exported as pdf

ChatGPT was used to help with extracting data from the Benchmark results into the tables.
There was a lot of raw data, doing it myself would have taken a long time.
What data to extract, how to display them and the conclusions drawn from them are entirely my work.
(Essentially the tables are generated)

# Information on the Implementation

## How the custom memory allocator works

This implementation uses mmap, not brk. I missread the exercise and somehow had
it in my head that we can use either brk or mmap. I probably picked this up
during research as I have seen both being used.
I only realized this once I after I was done with my implementation so I decided to keep it this way. As the same concept still apply regardless.

The allocator works as follows:
We use mmap to map a multiple of the kernel page size into the processes virtual address space.
The minimum size is 64 KiB, otherwise the requested bytes are rounded to the next multiple of page_size.

We call mapped memory an arena.
Each Arena has an aligned header containing, its size, pointers to the next/ previous arena Header.
Each Arena consist of blocks.
A block has an aligned Header and a payload.
A block contains pointer to its direct neighbors inside an arena, and pointers to the the next and previous free block (if it is a free block, otherwise the pointers are NULL).

Arena layout:
```text
[arena header][block header][payload][block header][payload] ...
```

```c
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
```

When memory is requested:

1. Try to find a matching block in the free list (according to the search strategy)
2. If the block is found, remove it from the free list. If the block is larger than the
   requested size, split the remainder into its own block and add it to the head of the free list.
3. If no large enough block is found, map a new arena, and use it to allocate a suitable block

When memory is freed:

1. Check if its direct physical neighbors are also free and merge them into 1 block
2. Add that block to the Head of the free list
   A free block will ALWAYS have allocated neighbors or no neighbors at all. (As they would be merged otherwise)

## Some caveats

Free blocks are managed by a doubly linked list of free blocks. This list contains blocks
across individual arenas.
Whenever a block is freed it is added to the Head of that linked list.
This means the search order for the different strategies does not depend on physical location but on when it was last freed.

Arena can be unmapped once it contains no more allocated blocks. Since the free list spans across arenas, it is unlikely that this happens and therefore once an arena is mapped it is probably not unmapped, unless the arena only contained one large block to begin with.

A better approach may have been to have a free list per arena and than try to implement the allocator in a way that maintains a fixed search order for the arena, such that over time arenas at the end of the list tend to be allocated less than arena at the front, so that its more likely that they are freed. But this is speculation.

There is 2 version of the custom memory allocator: plain and with stats.
The version with stats keeps track of allocation statistics later used in benchmarks and also exposes a validation function for unittesting.

The plain version only contains the memory allocation, so it can be used to compare directly to glibc.

## Data strucuture efficience.

Using a doubly linked list for managing free blocks and nieghbors allows us to insert and remove blocks
from the free list and neighbor list in constant time. However this means that each block header contains
at least 4 pointers. On a 64 bit system that is $4 \times  8 = 32 Bytes$. (In practice its more because of the arena pointer and size information)
So the best case scenario for a program is when the program frequently allocates and frees large blocks of memory.
Worst case is infrequent allocation/ freeing of small blocks of data.

# Benchmarking

## Benchmark details

All 3 allocators (glibc, plain, with stats) where benchmarked using repeatable seeded workloads.
Each workload contains 1024 slots. So a maximum of 1024 simultanious allocations are happening during the workload.

```c
struct {
    unsigned char *ptr;    // pointer to allocated memory
    size_t size;           // size of allocated memory
    unsigned char pattern; // expected value of bytes in slot
} slots[1024] = {{0}};
```

Each workload Runs for a set number of iteration. Each iteration contains 1 call to an allocator function based on values
derived from the seed using an xorshift32 PRNG.

The xorshift32 PRNG chooses slot, operation, and size inputs. Empty slots in uniform, mixed,
and fragmented use calloc when choice % 4 == 0, otherwise malloc.

Mixed selects large requests when size_bits % 8 == 0, then uses 4096 + size_bits % 32768; otherwise it uses 1 + size_bits % 2048.
large requests are multiples of 8 and top out at 36,856 bytes.

Every size, including fragmented setup, is multiplied by the allocation scale. Scaling does not change random draws or allocation/free choices.

| Workload   | Requests and lifetime pattern                                                                | Base request sizes (bytes)                               |
| ---------- | -------------------------------------------------------------------------------------------- | -------------------------------------------------------- |
| uniform    | An occupied selected slot is freed; an empty slot is allocated                               | 64                                                       |
| mixed      | Same allocation/free decisions; a mix of small and large requests                            | 2–2,048 small; 4,096–36,856 large (see below)            |
| fragmented | Allocate all slots, free even slots; keep odd slots live while churning even slots           | Setup: 64 + (index % 8) × 128; replacements: 1,024–3,071 |
| resize     | Free an occupied slot when choice % 4 == 0; otherwise realloc, including realloc(NULL, size) | 1–16,384                                                 |

| Scale | Uniform | Mixed small bounds | Mixed large bounds | Fragmented setup bounds | Fragmented replacement bounds | Resize bounds |
| ----- | ------- | ------------------ | ------------------ | ----------------------- | ----------------------------- | ------------- |
| 1     | 64      | 2–2,048            | 4,096–36,856       | 64–960                  | 1,024–3,071                   | 1–16,384      |
| 8     | 512     | 16–16,384          | 32,768–294,848     | 512–7,680               | 8,192–24,568                  | 8–131,072     |
| 16    | 1024    | 32–32,768          | 65,536–589,696     | 1,024–15,360            | 16,384–49,136                 | 16–262,144    |

The benchmark can be executed in 3 different ways:

| Mode         | Explanation                                                                                             |
| ------------ | ------------------------------------------------------------------------------------------------------- |
| single       | Benchmark is run a single time                                                                          |
| average      | Benchmark is run 10 times with the same seed. Results are averaged                                      |
| average-seed | Benchmark is is run 10 times with different seeds (derived from the initial seed). Results are averaged |

The following parameters where used, for the different configuration

| Scale | Mode         | Repetitions/config | Starting seed | Steps  | Slots | Configurations |
| ----- | ------------ | ------------------ | ------------- | ------ | ----- | -------------- |
| 1     | single       | 1                  | 12345         | 100000 | 1024  | 28             |
| 1     | average      | 10                 | 12345         | 100000 | 1024  | 28             |
| 1     | average-seed | 10                 | 12345         | 100000 | 1024  | 28             |
| 8     | single       | 1                  | 12345         | 100000 | 1024  | 28             |
| 8     | average      | 10                 | 12345         | 100000 | 1024  | 28             |
| 8     | average-seed | 10                 | 12345         | 100000 | 1024  | 28             |
| 16    | single       | 1                  | 12345         | 100000 | 1024  | 28             |
| 16    | average      | 10                 | 12345         | 100000 | 1024  | 28             |
| 16    | average-seed | 10                 | 12345         | 100000 | 1024  | 28             |

Each workload is benchmarked in 7 different configuration

1. glibc
2. allocator.c (3 times once for each strategy)
3. allocator_with_stats.c (3 times once for each strategy)

The benchmark records metrics:

1. Before the workload is run
2. Immediately after the workload ran
3. After cleanup ran

Unless otherwise specified we are using 2. 
A benchmark returns the following data:

```json
{
  "schema_version": 1,
  "workload": "uniform",
  "allocator": "system",
  "variant": "glibc",
  "allocator_library": "/lib/aarch64-linux-gnu/libc.so.6",
  "seed": 1,
  "steps": 1,
  "size_scale": 1,
  "slots": 1024,
  "elapsed_ns": 5000,
  "throughput_calls_per_second": 200000,
  "operations": {
    "total": 1,
    "malloc": 1,
    "calloc": 0,
    "realloc": 0,
    "free": 0
  },
  "live_requested_bytes": 64,
  "peak_live_requested_bytes": 64,
  "checksum": 301,
  "cleanup_free_calls": 1,
  "checkpoints": {
    "initial": {
      "rss_kib": 1200,
      "peak_rss_kib": 1200,
      "allocator_stats": null
    },
    "before_cleanup": {
      "rss_kib": 1204,
      "peak_rss_kib": 1204,
      "allocator_stats": null
    },
    "after_cleanup": {
      "rss_kib": 1204,
      "peak_rss_kib": 1204,
      "allocator_stats": null
    }
  }
}
```

`rss_kib` and `peak_rss_kib` are optained from:

- /proc/self/smaps_rollup: Rss;
- /proc/self/status: VmHWM;

allocator_stats is a json representation of allocator_stats, which is optained directly from the allocator:
(if the library with stats is used)

```c
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

```

# Evaluation

The evaluation only uses a sub set of the ran benchmarks. Evaluating all data would be too much effort

## Evaluating performance between glibc and allocator.c utilizing different strategies

Since we cannot collect internal allocator data for glibc, we will be using allocator.c and do not consider allocator_with_stats.c. (Stats collection impacts performance, skewing results).

Therefore we can only evaluate execution time, and RSS (resident set size) accross different workloads and scales

The following comparison uses the `average` results: 10 runs with seed 12345,
100,000 steps, and 1,024 slots, at scales 1, 8, and 16

**Bold marks the best custom strategy in each row:** lowest execution time in
the time table and lowest RSS in the memory table.

**Mean execution time in milliseconds (lower is better).** Parentheses show
custom time divided by glibc time: below 1× is faster than glibc; above 1× is slower.
The timer includes allocation calls, PRNG calculations, payload checks, and writes,
but excludes final cleanup and memory snapshots.

| Scale | Workload   | glibc ms | First Fit ms (ratio) | Best Fit ms (ratio) | Next Fit ms (ratio) |
| ----- | ---------- | -------- | -------------------- | ------------------- | ------------------- |
| 1     | uniform    | 2.094    | 2.691 (1.28×)        | **2.542 (1.21×)**   | 2.908 (1.39×)       |
| 1     | mixed      | 8.426    | **8.583 (1.02×)**    | 23.596 (2.80×)      | 8.779 (1.04×)       |
| 1     | fragmented | 6.532    | **5.445 (0.83×)**    | 62.102 (9.51×)      | 5.731 (0.88×)       |
| 1     | resize     | 47.112   | **36.277 (0.77×)**   | 117.546 (2.50×)     | 38.870 (0.83×)      |
| 8     | uniform    | 2.698    | 3.009 (1.12×)        | 3.103 (1.15×)       | **3.001 (1.11×)**   |
| 8     | mixed      | 58.832   | 181.825 (3.09×)      | **112.801 (1.92×)** | 165.829 (2.82×)     |
| 8     | fragmented | 22.763   | **24.879 (1.09×)**   | 218.752 (9.61×)     | 25.241 (1.11×)      |
| 8     | resize     | 428.168  | **875.421 (2.04×)**  | 933.726 (2.18×)     | 886.457 (2.07×)     |
| 16    | uniform    | 3.147    | **3.494 (1.11×)**    | 3.578 (1.14×)       | 3.498 (1.11×)       |
| 16    | mixed      | 112.041  | 398.520 (3.56×)      | **255.177 (2.28×)** | 380.512 (3.40×)     |
| 16    | fragmented | 43.058   | **186.831 (4.34×)**  | 418.905 (9.73×)     | 224.892 (5.22×)     |
| 16    | resize     | 841.440  | **1600.686 (1.90×)** | 1613.414 (1.92×)    | 1614.043 (1.92×)    |

**Mean RSS before cleanup in MiB (lower is better).** These are absolute resident-memory snapshots while the remaining allocations are still live (right after the workload ran but before cleanup ran).

Parentheses show custom RSS divided by glibc RSS for the same workload and scale.

| Scale | Workload   | glibc MiB | First Fit MiB (ratio) | Best Fit MiB (ratio) | Next Fit MiB (ratio) |
| ----- | ---------- | --------- | --------------------- | -------------------- | -------------------- |
| 1     | uniform    | 1.137     | 1.268 (1.11×)         | **1.249 (1.10×)**    | 1.295 (1.14×)        |
| 1     | mixed      | 3.950     | 5.396 (1.37×)         | **3.741 (0.95×)**    | 5.114 (1.29×)        |
| 1     | fragmented | 2.249     | 2.490 (1.11×)         | **2.390 (1.06×)**    | 2.490 (1.11×)        |
| 1     | resize     | 10.391    | 11.678 (1.12×)        | **9.518 (0.92×)**    | 11.486 (1.11×)       |
| 8     | uniform    | 1.376     | 1.489 (1.08×)         | **1.488 (1.08×)**    | 1.490 (1.08×)        |
| 8     | mixed      | 22.470    | 22.151 (0.99×)        | **19.395 (0.86×)**   | 22.000 (0.98×)       |
| 8     | fragmented | 9.739     | 10.955 (1.12×)        | **9.797 (1.01×)**    | 10.796 (1.11×)       |
| 8     | resize     | 71.959    | 69.603 (0.97×)        | **64.470 (0.90×)**   | 69.923 (0.97×)       |
| 16    | uniform    | 1.655     | 1.802 (1.09×)         | **1.765 (1.07×)**    | 1.798 (1.09×)        |
| 16    | mixed      | 41.443    | 41.181 (0.99×)        | **38.431 (0.93×)**   | 41.842 (1.01×)       |
| 16    | fragmented | 18.466    | 19.584 (1.06×)        | **18.580 (1.01×)**   | 19.603 (1.06×)       |
| 16    | resize     | 143.272   | 138.279 (0.97×)       | **127.457 (0.89×)**  | 136.528 (0.95×)      |

At scale 1, First and Next Fit outperform glibc in the fragmented and resize workloads. First Fit is about 17% faster for fragmented allocations and 23% faster for resize.

In mixed allocations it is close to glibc, while glibc is faster for uniform allocations. At scales 8 and 16, glibc is faster in every measured case.
The custom allocator's advantage at small sizes does not carry over to larger
reqeusts.

The reason for this may be found in the recorded nr of calls to mmap.
At scale 1 we are making 359 calls to mmap where as at **16** we are doing **8266** $8266 / 359 = 23$ which is more than a 16x increase. So I assume that glibc probably uses better heuristics to determine the size of its mmap calls, so it probably makes less syscalls.

Best Fit illustrates a tradeoff between search cost and memory reuse. Unless it finds an exact fit, it scans the entire free list to find the smallest suitable block. First and Next Fit stop at the first suitable block they encounter.

Best Fit is particularly slow in the fragmented workload: about 9.5–9.7 times glibc's time. Since fragementation leads to more free blocks, which increases search time. However, it has the lowest RSS among the custom strategies in every row. And has better RSS than glibc in mixed and resize workloads at all scales.

Selecting smaller suitable blocks can leave larger blocks available for later requests. Which may result in better fragementation behavior. This becomes more noticable at larger scales, because the allocation sizes get closer to the the default arena size of 64 KiB. (Which means less blocks per arena and therefore more mapped pages)

This may explain why Best Fit becomes the fastest custom strategy for mixed workloads at scales 8 and 16, despite its more expensive search.

Looking at the nr of mmap calls at different scales for different workloads supports this:

| Workload   | Scale | First Fit | Best Fit | Next Fit |
| ---------- | ----: | --------: | -------: | -------: |
| Mixed      |     1 |       114 |       93 |      150 |
| Mixed      |     8 |     4,280 |      852 |    3,700 |
| Mixed      |    16 |     5,517 |    1,433 |    4,797 |
| Fragmented |     1 |        21 |       20 |       21 |
| Fragmented |     8 |       359 |      465 |      315 |
| Fragmented |    16 |     8,266 |    4,407 |    8,715 |
| Resize     |     1 |       169 |      153 |      166 |
| Resize     |     8 |    17,937 |   12,459 |   18,008 |
| Resize     |    16 |    18,317 |   12,631 |   18,380 |

We can also again see that using best fit seems to utilize the available memory better, since less memory pages need to be mapped. However it is offset by the search time

Best Fit has a lower RSS or comparable RSS accross scales and workloads compared to glibc.

## Evaluating Fragementation and memory efficiency in allocator_with_stats.c

To evaluate fragmentation we need knowledge of the internal memory layout of the allocator.
Since we do not have this data for glibc and allocator.c we will only evaluate allocator_with_stats.c

Fragmentation can be either internal or external:

**Internal fragmentation**: Internal fragmentation is the amount of unused memory inside an allocated block.
Since the Block Header addresses need to be aligned, there can be a difference between the space a block consumes inside an arena and the size that was actually requested.

We can measure this directly in our custom allocator by subtracting a blocks requested size from its capacity. We call this measure the **Internal Slack**.

**External fragmentation**: Is the amount of space that exists, but is split across blocks that are individually too small to serve a request. 
We have no direct way of measuring this but we can make guesses based on
- the nr of free bytes in an arena
- the nr of free block
- the average free block size
- the capacity of the largest free block.

Finally, headers and alignment padding add overhead. We calculate it by subtracting the payload capacity of live blocks and free blocks from the total mapped memory:
overhead = mapped_bytes − capacity_bytes − free_bytes

### Internal Fragmentation

This table uses the `average-seeds` benchmark results: ten runs with different,
deterministically. Each cell shows internal slack in KiB and as a percentage of live requested bytes.
Slack is `capacity_bytes - requested_bytes`.
The percentage displayed is the Slack devides by the requested_bytes.
This gives us a measure of the average extra capacity relativ to the requested bytes (lower is better)
The best value per row is **bold**

| Scale | Workload   |   First Fit: KiB (%) | Best Fit: KiB (%) |    Next Fit: KiB (%) |
| ----: | ---------- | -------------------: | ----------------: | -------------------: |
|     1 | mixed      |     3.556 (0.19173%) |  7.870 (0.42437%) | **3.502 (0.18887%)** |
|     1 | fragmented | **2.098 (0.26424%)** |  3.733 (0.47006%) |     2.195 (0.27644%) |
|     1 | resize     |     6.133 (0.09434%) | 10.320 (0.15876%) | **6.076 (0.09347%)** |
|     8 | mixed      |     1.555 (0.01048%) |  4.030 (0.02717%) | **1.526 (0.01028%)** |
|     8 | fragmented | **1.079 (0.01698%)** |  1.446 (0.02276%) |     1.102 (0.01735%) |
|     8 | resize     | **3.324 (0.00639%)** |  5.115 (0.00984%) |     3.373 (0.00649%) |
|    16 | mixed      | **0.078 (0.00026%)** |  1.966 (0.00662%) |     0.084 (0.00028%) |
|    16 | fragmented |     0.064 (0.00050%) |  0.598 (0.00471%) | **0.045 (0.00036%)** |
|    16 | resize     | **0.072 (0.00007%)** |  1.056 (0.00102%) |     0.092 (0.00009%) |

The uniform workload has no slack at any scale because the requested size of `64 Bytes * scale` always matches the alignment. We therefore drop it from the table.

Surprisingly best fit seems to never lead to the best internal fragmentation. I assume the reason for this is as follows:
Best fit chooses the smallest available block for a given request size. 
This means the remainder of the block is likely to be too small to be split into its own block.
Which leads to internal fragmentation.
Where as next fit and first fit do not care about the block size,
which means its more likely that the block is alrge enough to be split into its block.

The comparison between first fit and next fit is inconclusive, it seems to depend on the scale and workload.
For example fragmented1 and fragemented8 first fit performs better and for fragemented16 next fit performs better.

Further testing would be required, maybe its also just up the particular seeds being used.

### External Fragmentation

We use the same ten different seeds, 100,000 steps, and 1,024 slots as in the
internal-fragmentation table. Each free block is a separate region that can serve
one request; blocks in different arenas cannot be merged.

The **free-space dispersion** score is
`100 × (1 − largest_free_block / free_bytes)`. It shows what percentage of free
payloads lies outside the largest free block. Lower values means more free space
is concentrated in one block; **bold** marks the lowest score for each workload
and scale. Lower is better. (One bold entry per 3 rows)

Free MiB is the amound of free space accross arenas. Average Free KiB and Largest free KiB
are the average free block size and largest free block size.

| Scale | Workload   | Strategy | Free MiB | Free blocks | Avg. free KiB | Largest free KiB | Dispersion |
| ----: | ---------- | -------- | -------: | ----------: | ------------: | ---------------: | ---------: |
|     1 | mixed      | first    |    2.221 |       301.8 |          7.53 |            51.88 |      97.7% |
|     1 | mixed      | best     |    0.640 |       134.9 |          4.86 |            61.08 |  **90.7%** |
|     1 | mixed      | next     |    2.027 |       298.9 |          6.94 |            48.12 |      97.7% |
|     1 | fragmented | first    |    0.454 |       645.3 |          0.72 |            10.78 |      97.7% |
|     1 | fragmented | best     |    0.386 |       606.5 |          0.65 |            61.33 |  **84.5%** |
|     1 | fragmented | next     |    0.467 |       642.1 |          0.74 |            13.22 |      97.2% |
|     1 | resize     | first    |    4.038 |       652.8 |          6.34 |            42.09 |      99.0% |
|     1 | resize     | best     |    1.923 |       438.9 |          4.49 |            55.87 |  **97.2%** |
|     1 | resize     | next     |    3.932 |       654.0 |          6.16 |            48.39 |      98.8% |
|     8 | mixed      | first    |    7.350 |       324.8 |         23.13 |           143.49 |      98.1% |
|     8 | mixed      | best     |    4.122 |       230.9 |         18.36 |           269.45 |  **93.6%** |
|     8 | mixed      | next     |    6.709 |       320.0 |         21.46 |           136.22 |      98.0% |
|     8 | fragmented | first    |    3.550 |       720.4 |          5.05 |            45.96 |      98.7% |
|     8 | fragmented | best     |    2.387 |       624.3 |          3.91 |            55.54 |  **97.7%** |
|     8 | fragmented | next     |    3.381 |       717.6 |          4.82 |            52.75 |      98.5% |
|     8 | resize     | first    |   18.824 |       872.1 |         22.10 |            86.82 |      99.5% |
|     8 | resize     | best     |   13.478 |       754.8 |         18.28 |           100.05 |  **99.3%** |
|     8 | resize     | next     |   18.719 |       865.9 |         22.13 |            85.70 |      99.6% |
|    16 | mixed      | first    |   10.697 |       320.5 |         34.16 |           241.73 |      97.8% |
|    16 | mixed      | best     |    7.122 |       257.5 |         28.27 |           507.70 |  **93.0%** |
|    16 | mixed      | next     |   11.455 |       316.5 |         37.08 |           248.90 |      97.9% |
|    16 | fragmented | first    |    6.822 |       749.5 |          9.32 |            42.56 |      99.4% |
|    16 | fragmented | best     |    4.571 |       659.7 |          7.09 |            55.88 |  **98.8%** |
|    16 | fragmented | next     |    6.503 |       743.9 |          8.95 |            41.53 |      99.4% |
|    16 | resize     | first    |   37.240 |       872.4 |         43.70 |           176.69 |      99.5% |
|    16 | resize     | best     |   26.546 |       778.9 |         34.89 |           204.12 |  **99.2%** |
|    16 | resize     | next     |   36.067 |       864.0 |         42.74 |           173.31 |      99.5% |

Best Fit has the lowest dispersion accross all workloads and scales.

Best Fit also has fewer free blocks. This may result from fewer splits, more merging of blocks, or fewer mapped arenas.

All strategies process the same requests, so they have the same live requested bytes after the workload ran.

Best Fit’s lower Free MiB together with lower free block count supports the conclusion that it retains less arena space.
This is particularly noticeabe in the fragmented1 workload. In the higher scales the advantage is reduced, I habe not figured out a good reason as to why that is.

Overall all strategies are performing rather poorly, since the average dispersion across workloads and scales is very similar and >98% which means there is a lot of external fragmentation. However best fit still performs the best.

| Strategy  | Mean dispersion |
| --------- | --------------: |
| First Fit |          98.61% |
| Best Fit  |          94.90% |
| Next Fit  |          98.51% |

### Overhead

Headers and mapping padding occupy space that is neither live-block capacity
nor free-block capacity. Using the `average-seeds` results at `before_cleanup`,
we calculate `overhead = mapped_bytes - capacity_bytes - free_bytes`. Each cell
shows the overhead in KiB and as a share of mapped bytes. The percentage is
calculated from the averaged fields. **Bold** marks the lowest overhead in KiB
for each workload and scale.

| Scale | Workload   | First Fit: KiB (% mapped) | Best Fit: KiB (% mapped) | Next Fit: KiB (% mapped) |
| ----: | ---------- | ------------------------: | -----------------------: | -----------------------: |
|     1 | uniform    |           39.063 (30.52%) |      **33.569 (26.23%)** |          43.119 (33.69%) |
|     1 | mixed      |            53.569 (1.28%) |       **42.344 (1.65%)** |           53.291 (1.34%) |
|     1 | fragmented |            88.928 (6.59%) |       **86.469 (6.76%)** |           88.734 (6.51%) |
|     1 | resize     |            97.344 (0.91%) |       **82.913 (0.97%)** |           97.366 (0.92%) |
|     8 | uniform    |           36.575 (10.58%) |       **33.950 (9.82%)** |          36.831 (10.66%) |
|     8 | mixed      |            56.006 (0.25%) |       **49.531 (0.26%)** |           55.556 (0.26%) |
|     8 | fragmented |            97.887 (0.97%) |       **91.297 (1.03%)** |           97.628 (0.98%) |
|     8 | resize     |           125.247 (0.18%) |      **115.769 (0.18%)** |          124.869 (0.18%) |
|    16 | uniform    |            37.169 (5.81%) |       **34.325 (5.36%)** |           37.200 (5.81%) |
|    16 | mixed      |            55.775 (0.14%) |       **51.650 (0.14%)** |           55.359 (0.13%) |
|    16 | fragmented |           104.447 (0.53%) |       **97.706 (0.56%)** |          103.938 (0.53%) |
|    16 | resize     |           125.397 (0.09%) |      **117.319 (0.09%)** |          124.719 (0.09%) |

Best Fit has the lowest absolute overhead in every row. This is because as we discovered before, best fit seems to result in fewer total blocks, and therefore less overhead.

The uniform workload is performing the worst here, because it only does small allocations of 64 bytes. Which results in a large overhead.
This gets a lot better as scale increases because the ratio between header size and payload size improves.

# Conclusion

Best fit seems to result in better usage of memory in almost all cases.
However it can have significant performance losses especially in fragmented workloads. This improves as the average request size gets larger.

So if you are working in a very memory restricted environment, but are also predominantly doing larger allocations using best-fit might be a good option.
Otherwise the performance loss may not be worth it, especially in heavily fragmented workloads.

The performance of first and next fit in larger allocations can probably be improved, by chosing a larger default arena size, or better yet scaling the arena size with the request size.
For example: $\operatorname{round}\!\left(\max\!\left(\text{min\_arena\_size},\;4 \times \text{requested\_bytes}\right)\right)$
An analysis of the average arena size may be helpful here.

Currently a larger request requests an arena that is only slightly larger than the requested size (due to it being rounded to the next multiple of page size).
Which means the new arena won't have space for additional allocations, which means the next malloc call will need to map another arena and so on.
Which removes the performance gains from mapping larger chunks of memory in advance.

glibc (as expected) outperforms the custom allocator in almost all scenarios
(fragmentation not measured, but RSS is still better in almost all scenarios).

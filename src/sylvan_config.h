/* Operation cache: use bitmasks for module (size must be power of 2!) */
#ifndef CACHE_MASK
#define CACHE_MASK 1
#endif

/* Nodes table: use bitmasks for module (size must be power of 2!) */
#ifndef LLMSSET_MASK
#define LLMSSET_MASK 1
#endif

/**
 * Use Fibonacci sequence as resizing strategy.
 * This MAY result in more conservative memory consumption, but is not
 * great for performance.
 * By default, powers of 2 should be used.
 * If you set this, then set CACHE_MASK and LLMSSET_MASK to 0.
 */
#ifndef SYLVAN_SIZE_FIBONACCI
#define SYLVAN_SIZE_FIBONACCI 0
#endif

/* Enable/disable counters and timers */
#ifndef SYLVAN_STATS
#define SYLVAN_STATS 0
#endif

/* Enable/disable using mmap to allocate large amounts of memory */
#ifndef SYLVAN_USE_MMAP
#define SYLVAN_USE_MMAP 0
#endif

/* Aggressive or conservative resizing strategy */
#ifndef SYLVAN_AGGRESSIVE_RESIZE
#define SYLVAN_AGGRESSIVE_RESIZE 1
#endif

/* Either use chaining or linear implementation as a hash collision strategy */
#ifndef SYLVAN_USE_LINEAR_PROBING
#define SYLVAN_USE_LINEAR_PROBING 0
#endif

// Variable ordering default parameter values
#define SYLVAN_REORDER_MAX_VAR		    100
#define SYLVAN_REORDER_MAX_SWAPS	    10000
#define SYLVAN_REORDER_GROWTH	        1.2f
#define SYLVAN_REORDER_NODES_THRESHOLD	1
#define SYLVAN_REORDER_TIME_LIMIT_MS	(10000 * 60 * 1000)
#define SYLVAN_REORDER_SIZE_THRESHOLD	4000
#define SYLVAN_REORDER_SIZE_RATIO	    (2)
#define SYLVAN_REORDER_LIMIT	        10                              // maximum number of reordering calls allowed
#define SYLVAN_REORDER_TYPE_DEFAULT     SYLVAN_REORDER_BOUNDED_SIFT
#define SYLVAN_REORDER_PRINT_STAT       0
#define SYLVAN_REORDER_MIN_MEM_REQ      (0.9f)
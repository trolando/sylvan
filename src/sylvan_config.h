/* Operation cache: use bitmasks for module (size must be power of 2!) */
#ifndef CACHE_MASK
#define CACHE_MASK 1
#endif

/* Nodes table: use bitmasks for module (size must be power of 2!) */
#ifndef LLMSSET_MASK
#define LLMSSET_MASK 1
#endif

/* For Sylvan BDD: keep statistics of operation cache use */
#ifndef SYLVAN_CACHE_STATS
#define SYLVAN_CACHE_STATS 0
#endif

/* For Sylvan BDD: keep statistics of operations */
#ifndef SYLVAN_OPERATION_STATS
#define SYLVAN_OPERATION_STATS 0
#endif

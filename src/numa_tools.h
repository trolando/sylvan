#ifndef NUMATOOLS_H
#define NUMATOOLS_H

/**
 * Returns number of available cpus
 */
int numa_count_cpus_per_node(int *nodes, int *cpus);

/**
 * Calculates the number of currently available cpus
 */
int numa_available_cpus();

/**
 * Calculate the number of currently available nodes
 */
int numa_available_nodes();
    
/**
 * Calculate the number of available nodes for memory allocation
 */
int numa_available_memory_nodes();

/**
 * Calculate highest node number
 */
int numa_highest_node();

int numa_worker_info(int worker, int *node, int *node_index, int *index, int *total);

int numa_bind_me(int worker);

int numa_distribute(int workers);

/**
 * Move some piece of memory to some memory domain.
 * mem should be on a <getpagesize()> boundary!
 */
int numa_move(void *mem, size_t size, int node);

/**
 * This function distributes a preallocated array of <size> bytes
 * over all available NUMA memory domains. 
 *
 * Note that mem should be aligned on a <getpagesize()> boundary!
 *
 * On success, the array will be distributed over all memory nodes.
 * If <fragment_size> is not null, it will contain the size of each fragment.
 * The fragment size is rounded up to the page size.
 *
 * For example, if domains 0,3,6 are available, with 20000 bytes and page size of 4096,
 * then the first 8192 bytes are bound to domain 0, the next 8192 bytes to domain 3 and
 * the final 3616 bytes are bound to domain 6.
 * 
 * Returns 0 on success, -1 on failure.
 */
int numa_interleave(void *mem, size_t size, size_t *fragment_size);

#endif

/*
 * Copyright 2013-2014 Formal Methods and Tools, University of Twente
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef NUMATOOLS_H
#define NUMATOOLS_H

/**
 * Initializes / refreshes internal data
 */
int numa_tools_refresh(void);

/**
 * Get the number of configured cpus on this system
 */
size_t get_num_cpus(void);

/**
 * Returns an array per configured cpu which node (-1 if cpu not usable)
 */
const int *get_cpu_to_node(void);

size_t numa_available_cpus(); // number of available cores
size_t numa_available_work_nodes(); // number of nodes with available cores
size_t numa_available_memory_nodes(); // number of nodes with memory allocation

/**
 * Check if all nodes that host available cpus can also allocate memory
 * Returns 1 if this is the case, 0 otherwise.
 */
int numa_check_sanity(); // check that all work nodes are also memory nodes

/**
 * Calculate a distribution of N workers
 * Returns 0 if successful.
 */
int numa_distribute(size_t workers);

/**
 * Retrieve info for a certain worker
 * Retrieves - the node the worker is on
 *           - the index of our node (only count available nodes)
 *           - our worker index on the node
 *           - the total number of workers on this node
 */
int numa_worker_info(size_t worker, size_t *node, size_t *node_index, size_t *index, size_t *total);

/**
 * Bind me (worker) according to the calculated distribution
 */
int numa_bind_me(size_t worker);

/**
 * Move some piece of memory to some memory domain.
 * mem should be on a getpagesize() boundary!
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

/**
 * Retrieves the domain of a memory address... -1 if we could not retrieve
 */
int numa_getdomain(void *ptr);

/**
 * Returns <0 on error move_pages
 * or 0 if not all pages on the right domain
 * or 1 if they are on the expected domain
 */
int numa_checkdomain(void *ptr, size_t size, size_t expected_node);

#endif

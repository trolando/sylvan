#include <numa.h> // various numa functions
#include <numaif.h> // mpol
#include <sys/mman.h> // mmap
#include <unistd.h> // getpagesize
#include <stdio.h> // printf

static int *cpu_to_node = NULL;
static int num_cpus = 0;
static int *node_mem = NULL;
static int num_nodes = 0;
static int inited = 0;
static size_t pagesize = 0;

int
numa_tools_refresh(void)
{
    pagesize = getpagesize();

    num_cpus = numa_num_configured_cpus();
    if (cpu_to_node != NULL) free(cpu_to_node);
    cpu_to_node = calloc(num_cpus, sizeof(int));

    struct bitmask *cpubuf;
    int i;

    cpubuf = numa_allocate_cpumask();
    if (numa_sched_getaffinity(0, cpubuf) < 0) {
        numa_free_cpumask(cpubuf);
        return 0;
    }
    for (i=0; i<cpubuf->size && i<num_cpus; i++) {
        cpu_to_node[i] = numa_bitmask_isbitset(cpubuf, i) ? numa_node_of_cpu(i) : -1;
    }
    numa_free_cpumask(cpubuf);

    num_nodes = numa_max_node()+1;
    if (node_mem != NULL) free(node_mem);
    node_mem = calloc(num_nodes, sizeof(int));

    struct bitmask *allowed = numa_allocate_nodemask();
    if (get_mempolicy(NULL, allowed->maskp, allowed->size+1, 0, MPOL_F_MEMS_ALLOWED) != 0) {
        numa_bitmask_free(allowed);
        return 0;
    }
    for (i=0; i<allowed->size && i<num_nodes; i++) {
        node_mem[i] = numa_bitmask_isbitset(allowed, i) ? 1 : 0;
    }
    numa_bitmask_free(allowed);

    inited = 1;

    /*
    printf("There are at most %d cpus and at most %d nodes.\n", num_cpus, num_nodes);
    printf("Available nodes (memory and program execution):\n");
    for (i=0; i<num_nodes; i++) {
        if (!node_mem[i]) continue;
        int j, k;
        for (j=k=0; j<num_cpus; j++) if (cpu_to_node[j]==i) k++;
        printf("- node %d (%d cpus)\n", i, k);
    }
    printf("Available nodes for program execution only:\n");
    for (i=0; i<num_nodes; i++) {
        if (node_mem[i]) continue;
        int j,k=0;
        for (j=k=0; j<num_cpus; j++) if (cpu_to_node[j]==i) k++;
        if (k>0) printf("- node %d (%d cpus)\n", i, k);
    }
    */

    return 1;
}

int
get_num_cpus(void)
{
    return num_cpus;
}

const int*
get_cpu_to_node(void)
{
    return cpu_to_node;
}

static int
numa_tools_init()
{
    if (inited) return 1;
    numa_tools_refresh();
    return inited;
}

/**
 * Returns number of available cpus
 */
int
numa_cpus_per_node(int *nodes)
{
    if (!numa_tools_init()) return 0;
    int i;
    for (i=0;i<num_cpus;i++) {
        if (cpu_to_node[i] != -1) nodes[cpu_to_node[i]]++;
    }
    return 1;
}


/**
 * Calculates the number of currently available cpus
 */
int
numa_available_cpus()
{
    if (!numa_tools_init()) return 0;
    int i,j=0;
    for (i=0;i<num_cpus;i++) {
        if (cpu_to_node[i] != -1) j++;
    }
    return j;
}

/**
 * Calculate the number of currently available nodes
 */
int
numa_available_work_nodes()
{
    if (!numa_tools_init()) return 0;
    int *nodes = (int*)alloca(sizeof(int)*num_nodes);
    memset(nodes, 0, num_nodes*sizeof(int));

    int i,j=0;
    for (i=0;i<num_cpus;i++) if (cpu_to_node[i]!=-1) nodes[cpu_to_node[i]]++;
    for (i=0;i<num_nodes;i++) j += nodes[i] > 0 ? 1 : 0;
    return j;
}

/**
 * Calculate the number of available nodes for memory allocation
 */
int
numa_available_memory_nodes()
{
    if (!numa_tools_init()) return 0;
    int i, j=0;
    for (i=0;i<num_nodes;i++) if (node_mem[i]) j++;
    return j;
}

/**
 * Check if every core is on a domain where we can allocate memory
 */
int
numa_check_sanity(void)
{
    if (!numa_tools_init()) return 0;
    int i, good = 1;
    for (i=0; i<num_cpus && good; i++) {
        if (cpu_to_node[i] != -1) {
            if (node_mem[cpu_to_node[i]] == 0) good = 0;
        }
    }
    return good;
}

static int n_workers = 0, n_nodes = 0;
static int *worker_to_node = 0;
static int *selected_nodes = 0;

int
numa_worker_info(int worker, int *node, int *node_index, int *index, int *total)
{
    *node = -1;

    if (worker < 0) return -1;
    if (worker >= n_workers) return -1;

    *node = worker_to_node[worker];

    int i,t=0;
    for (i=0; i<n_workers; i++) {
        if (i == worker && index != NULL) *index = t;
        if (worker_to_node[i] == *node) t++;
    }
    if (total != NULL) *total = t;

    if (node_index != NULL) {
        for (i=0; i<n_nodes; i++) {
            if (selected_nodes[i] == *node) { *node_index = i; break; }
        }
    }

    return 0;
}

int
numa_bind_me(int worker)
{
    int node, res;
    if ((res=numa_worker_info(worker, &node, 0, 0, 0)) != 0) return res;
    if ((res=numa_run_on_node(node)) != 0) return res;
    return 0;
}

int
numa_distribute(int workers)
{
    if (!numa_tools_init()) return 1;

    n_workers = workers;
    if (worker_to_node != 0) free(worker_to_node);
    worker_to_node = malloc(sizeof(int) * n_workers);

    int i,j;
    int *nodes = (int*)alloca(sizeof(int) * num_nodes);
    memset(nodes, 0, num_nodes*sizeof(int));
    for (i=0;i<num_cpus;i++) if (cpu_to_node[i]!=-1) nodes[cpu_to_node[i]]++;

    // Determine number of selected nodes
    int tot_nodes = 0;
    for (i=0;i<num_nodes;i++) if (nodes[i]>0) tot_nodes++;
    n_nodes = workers > tot_nodes ? tot_nodes : workers;

    // Get distances
    int *distances = (int*)malloc(num_nodes * num_nodes * sizeof(int));
    for (i=0; i<num_nodes; i++) for (j=0; j<num_nodes; j++) distances[num_nodes*i + j] = numa_distance(i, j);

    // Calculate best setup
    unsigned long best_setup = 0;
    double best_avgdist = 0;

    unsigned long setups = 1 << tot_nodes, setup; // 8 nodes: 256 setups
    for (setup=0;setup<setups;setup++) {
        // Only try setups that actually have the right number of nodes
        if (__builtin_popcount(setup) != n_nodes) continue;

        // Calculate cumulative distance
        int cumdist=0, links=0, k, all_available=1;
        long s=setup, t;
        j=0;
        while (s && all_available) {
            if (s&1) {
                if (nodes[j]==0) all_available = 0;
                t=setup;
                k=0;
                while (t) {
                    if (t&1) {
                        if (distances[tot_nodes*j + k] > 0) {
                            links++;
                            cumdist += distances[tot_nodes*j + k];
                        }
                    }
                    t>>=1; k++;
                }
            }
            s>>=1; j++;
        }

        if (all_available == 0) continue;

        // Keep best setup
        double d = (double)cumdist/(double)links;
        if (d == 0) continue;
        if (d < best_avgdist || best_avgdist == 0) {
            best_avgdist = d;
            best_setup = setup;
        }
    }

    free(distances);

    // Select nodes
    if (selected_nodes != 0) free(selected_nodes);
    selected_nodes = (int*)malloc(sizeof(int) * n_nodes);

    i = j = 0;
    while (best_setup) {
        if (best_setup&1) selected_nodes[j++] = i;
        best_setup >>= 1;
        i++;
    }

    // Distribute workers
    int count, *s_cpus = (int*)alloca(sizeof(int) * n_nodes);
    for (i=count=0; i<n_nodes; i++) count += (s_cpus[i] = nodes[selected_nodes[i]]);

    for (i=j=0; i<n_workers; i++) {
        if (count == 0) {
            int k;
            for (k=count=0; k<n_nodes; k++) count += (s_cpus[k] = nodes[selected_nodes[k]]);
        }
        while (s_cpus[j] <= 0) j = (j+1)%n_nodes;
        worker_to_node[i] = selected_nodes[j];
        s_cpus[j]--;
        count--;
        j = (j+1)%n_nodes;
    }

    return 0;
}

/**
 * Move some piece of memory to some memory domain.
 * mem should be on a <getpagesize()> boundary!
 */
int
numa_move(void *mem, size_t size, int node)
{
    struct bitmask *bmp = numa_allocate_nodemask();
    numa_bitmask_clearall(bmp);
    numa_bitmask_setbit(bmp, node);
    int res = mbind(mem, size, MPOL_BIND, bmp->maskp, bmp->size+1, MPOL_MF_MOVE);
    numa_bitmask_free(bmp);
    return res;
}

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
int numa_interleave(void *mem, size_t size, size_t *fragment_size)
{
    struct bitmask *allowed;
    int i, n_allowed, nodesleft;
    size_t f_size, offset;

    // Determine number of memory domains
    allowed = numa_allocate_nodemask();
    if (get_mempolicy(NULL, allowed->maskp, allowed->size+1, 0, MPOL_F_MEMS_ALLOWED) != 0) {
        numa_bitmask_free(allowed);
        return -1;
    }

    n_allowed=0;
    for (i=0;i<allowed->size;i++) {
        if (numa_bitmask_isbitset(allowed, i)) n_allowed++;
    }

    // Determine fragment size
    if (fragment_size != 0 && *fragment_size != 0) f_size = *fragment_size;
    else f_size = size / n_allowed;
    if (f_size != 0) {
        size_t ps1 = getpagesize()-1;
        f_size = (f_size + ps1) & ~ps1;
    } else {
        f_size = getpagesize();
    }

    // Bind all fragments
    offset = 0;
    nodesleft = n_allowed;
    for (i=0;i<allowed->size && offset < size;i++) {
        if (numa_bitmask_isbitset(allowed, i)) {
            size_t to_bind = f_size;
            if (--nodesleft == 0) {
                to_bind = size-offset;
            } else {
                if (to_bind > (size-offset)) to_bind = size-offset;
            }
            if (numa_move(mem+offset, to_bind, i) != 0) {
                numa_bitmask_free(allowed);
                return -1;
            }
            offset += to_bind;
        }
    }

    numa_bitmask_free(allowed);

    if (fragment_size != NULL) *fragment_size = f_size;
    return 0;
}


/**
 * This function allocates an array of <size> bytes and automatically distributes it
 * over all available NUMA memory domains.
 */
void *
numa_alloc_interleaved_manually(size_t size, size_t *fragment_size, int shared)
{
    char *mem;
    size_t f_size=0;

    if (shared)
        mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, 0, 0);
    else
        mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

    if (mem == (char*)-1) {
        return NULL;
    }

    if (shared) {
        size_t offset;
        for (offset = 0; offset < size; offset+=pagesize) {
            *((volatile char*)&mem[offset]) = 0;
        }
    }

    if (fragment_size != 0) f_size = *fragment_size;
    int res = numa_interleave(mem, size, &f_size);
    if (fragment_size != 0) *fragment_size = f_size;

    if (res == -1) {
        munmap(mem, size);
        mem = 0;
    }

    return mem;
}

int
numa_getdomain(void *ptr)
{
    ptr = (void*) ((size_t)ptr & ~(pagesize-1)); // align to page
    int status = -1;
    move_pages(0, 1, &ptr, NULL, &status, 0);
    return status;
}

int
numa_checkdomain(void *ptr, size_t size, int expected_node)
{
    size_t base = ((size_t)ptr & ~(pagesize-1));
    size_t last = ((((size_t)ptr) + size) & ~(pagesize-1));
    for (; base != last; base += pagesize) {
        int status = -1, res;
        if ((res = move_pages(0, 1, (void**)&base, NULL, &status, 0)) != 0) return res;
        if (status != expected_node) return 0;
    }

    return 1;
}

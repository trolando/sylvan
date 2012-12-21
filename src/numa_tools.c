#include <numa.h> // various numa functions
#include <numaif.h> // mpol
#include <sys/mman.h> // mmap
#include <unistd.h> // getpagesize

/**
 * Returns number of available cpus
 */
int numa_count_cpus_per_node(int *nodes, int *cpus)
{
    struct bitmask *cpubuf;
    int i, j;

    cpubuf = numa_allocate_cpumask();
    if (numa_sched_getaffinity(0, cpubuf) < 0) {
        numa_free_cpumask(cpubuf);
        return -1;
    }

    for (i=j=0; i<cpubuf->size; i++) {
        if (numa_bitmask_isbitset(cpubuf, i)) {
            if (nodes != NULL) nodes[numa_node_of_cpu(i)]++;
            if (cpus != NULL) cpus[j++] = i;
        }
    }

    numa_free_cpumask(cpubuf);
    return j;
}

/**
 * Calculates the number of currently available cpus
 */
int numa_available_cpus()
{
    struct bitmask *cpubuf;
    int i, j=-1;

    cpubuf = numa_allocate_cpumask();
    if (numa_sched_getaffinity(0, cpubuf) >= 0) {
        for (i=j=0; i<cpubuf->size; i++) {
            j += numa_bitmask_isbitset(cpubuf, i) ? 1 : 0;
        }
    }

    numa_free_cpumask(cpubuf);
    return j;
}

/**
 * Calculate the number of currently available nodes
 */
int numa_available_nodes()
{
    struct bitmask *cpubuf;
    int i, j=-1, nnodes, *nodes;
    
    nnodes = numa_num_configured_nodes();
    nodes = (int*)alloca(sizeof(int)*nnodes);
    memset(nodes, 0, nnodes*sizeof(int));

    cpubuf = numa_allocate_cpumask();
    if (numa_sched_getaffinity(0, cpubuf) >= 0) {
        for (i=0; i<cpubuf->size; i++) if (numa_bitmask_isbitset(cpubuf, i)) nodes[numa_node_of_cpu(i)]++;
        for (i=j=0; i<nnodes; i++) j += nodes[i] > 0 ? 1 : 0;
    }

    numa_free_cpumask(cpubuf);
    return j;
}

/**
 * Calculate the number of available nodes for memory allocation
 */
int numa_available_memory_nodes()
{
    struct bitmask *allowed;
    int i, j=-1;

    // Determine number of memory domains
    allowed = numa_allocate_nodemask();
    if (get_mempolicy(NULL, allowed->maskp, allowed->size+1, 0, MPOL_F_MEMS_ALLOWED) != 0) {
        numa_bitmask_free(allowed);
        return -1;
    }

    j=0;
    for (i=0;i<allowed->size;i++) {
        if (numa_bitmask_isbitset(allowed, i)) j++;
    }

    numa_bitmask_free(allowed);
    return j;
}

/**
 * Calculate highest node number
 */
int numa_highest_node()
{
    struct bitmask *cpubuf;
    int i, j=-1, node;
    
    cpubuf = numa_allocate_cpumask();
    if (numa_sched_getaffinity(0, cpubuf) >= 0) 
        for (i=0; i<cpubuf->size; i++) 
            if (numa_bitmask_isbitset(cpubuf, i)) {
                node = numa_node_of_cpu(i);
                if (node > j) j = node;
            }

    numa_free_cpumask(cpubuf);
    return j;
}

static int n_workers = 0, n_nodes = 0;
static int *worker_node = 0;
static int *selected_nodes = 0;

int numa_worker_info(int worker, int *node, int *node_index, int *index, int *total)
{
    *node = -1;

    if (worker < 0) return -1;
    if (worker >= n_workers) return -1;

    *node = worker_node[worker];

    int i,t=0;
    for (i=0; i<n_workers; i++) {
        if (i == worker && index != NULL) *index = t;
        if (worker_node[i] == *node) t++;
    }
    if (total != NULL) *total = t;

    if (node_index != NULL) {
        for (i=0; i<n_nodes; i++) {
            if (selected_nodes[i] == *node) { *node_index = i; break; }
        }
    }

    return 0;
}

int numa_bind_me(int worker)
{
    int node, res;
    if ((res=numa_worker_info(worker, &node, 0, 0, 0)) != 0) return res;
    if ((res=numa_run_on_node(node)) != 0) return res;
    return 0;
}

int numa_distribute(int workers)
{
    n_workers = workers;
    if (worker_node != 0) free(worker_node);
    worker_node = malloc(sizeof(int) * n_workers);
  
    int tot_nodes, *nodes, *distances, i, j;

    tot_nodes = numa_highest_node() + 1;
    nodes = (int*)alloca(sizeof(int) * tot_nodes);
    memset(nodes, 0, tot_nodes*sizeof(int));
    numa_count_cpus_per_node(nodes, NULL);

    // Determine number of selected nodes
    n_nodes = workers > tot_nodes ? tot_nodes : workers;

    // Get distances
    distances = (int*)calloc(tot_nodes * tot_nodes, sizeof(int));
    for (i=0; i<tot_nodes; i++) for (j=0; j<tot_nodes; j++)
        distances[tot_nodes*i + j] = numa_distance(i, j);

    // Calculate best setup
    long best_setup = -1;
    double best_avgdist = 0;
    long setups = 1 << tot_nodes, setup; // 8 nodes: 256 setups
    for (setup=0;setup<setups;setup++) {
        // Only try setups that actually have the right number of nodes
        if (__builtin_popcount(setup) != n_nodes) continue;
        // Calculate cumulative distance
        int cumdist=0, links=0, k;
        long s=setup, t;
        j=0;
        while (s) { 
            if (s&1) {
                t=setup;
                k=0;
                while (t) {
                    if (t&1 && distances[tot_nodes*j + k] > 0) { 
                        links++;
                        cumdist += distances[tot_nodes*j + k]; 
                    }
                    t>>=1; k++;
                }
            }
            s>>=1; j++;
        }

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
        worker_node[i] = selected_nodes[j];
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
int numa_move(void *mem, size_t size, int node)
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
void *numa_alloc_interleaved_manually(size_t size, size_t *fragment_size, int shared)
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
        for (offset = 0; offset < size; offset+=4096) {
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


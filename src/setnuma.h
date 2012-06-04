#ifndef SETNUMA_H
#define SETNUMA_H

#include <numa.h>

static inline int __setnuma_countnodes(int *nodes, int *cpus)
{
    struct bitmask *cpubuf = numa_allocate_cpumask();
    if (numa_sched_getaffinity(0, cpubuf) < 0) {
        numa_free_cpumask(cpubuf);
        return -1;
    }

    int i, j=0;
    for (i=0; i<cpubuf->size; i++) {
        if (numa_bitmask_isbitset(cpubuf, i)) {
            nodes[numa_node_of_cpu(i)]++;
            cpus[j++] = i;
        }
    }

    numa_free_cpumask(cpubuf);
    return j;
}

static inline int8_t *setnuma_calculate_best(int count) {
    int nnodes = numa_num_configured_nodes();
    int ncpus = numa_num_configured_cpus();
    int *nodes = (int*)alloca(sizeof(int)*nnodes);
    memset(nodes, 0, nnodes*sizeof(int));
    int *cpus = (int*)alloca(sizeof(int)*ncpus);
    int *distances = (int*)alloca(sizeof(int)*nnodes*nnodes);
    __setnuma_countnodes(nodes, cpus);

    int i,j;

    // Get distances
    for (i=0;i<nnodes;i++) for (j=0;j<nnodes;j++) {
        distances[nnodes*i+j] = numa_distance(i, j);
    }

    int best_setup = -1;
    int best_cumdist = 0; // assumption: increasing number of links will never increase avg dist
    int setups = 1 << nnodes;
    for (i=0;i<setups;i++) {
        // only setups that actually have all nodes usable
        {
            int s=i, good=1, j=0; 
            while (s&&good) { if (s&1 && nodes[j]==0) { good=0; } s>>=1; j++; }
            if (!good) continue;
        }
        // calculate number of cpus and cumulative distance
        int cpus_in_setup=0;
        int cumdist=0;
        {
            int s=i, j=0;
            while (s) { 
                if (s&1) {
                    cpus_in_setup += nodes[j];
                    int t=i,k=0;
                    while (t) {
                        if (t&1) { cumdist += distances[nnodes*j+k]; }
                        t>>=1; k++;
                    }
                }
                s>>=1; j++;
            }
        }
        // only setups with sufficient number of cpus
        if (cpus_in_setup < count) continue;
        // keep best setup
        if (cumdist < best_cumdist || best_cumdist == 0) {
            best_cumdist = cumdist; best_setup = i;
        }
    }
    if (best_setup == -1) return 0;
    // count number of nodes in best setup
    int nodes_in_best=0;
    {
        int s=best_setup;
        while (s) { nodes_in_best += (s&1); s>>=1; }
    }
    int8_t *result = (int8_t*)malloc(sizeof(int8_t)*(nodes_in_best+1));
    {
        int s=best_setup, j=0, i=0;
        while (s) { if (s&1) result[i++] = j; s>>=1; j++; }
        result[i] = -1;
    }
    return result;
}
/* 
 

static inline cpu_per_node(int node)
{
}

/ **
 * Find optimal arrangement based on numa_distance
 * /
static inline void fillit(int *nodes, int length, int ncpus) {
  
}

*/
#endif

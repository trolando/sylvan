#ifndef SYLVAN_BENCHMARKS_SYLVAN_MRC_H
#define SYLVAN_BENCHMARKS_SYLVAN_MRC_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define COUNTER16_T_MAX UINT16_MAX
#define COUNTER32_T_MAX UINT32_MAX

typedef uint16_t counter16_t;
typedef _Atomic(counter16_t) atomic_counter16_t;

typedef struct atomic_counters16_s
{
    atomic_counter16_t *container;
    size_t size;
} atomic_counters16_t;

void atomic_counters16_init(atomic_counters16_t* self, size_t new_size);

void atomic_counters16_deinit(atomic_counters16_t *self);

void atomic_counters16_add(atomic_counters16_t* self, size_t idx, int val);

counter16_t atomic_counters16_get(const atomic_counters16_t* self, size_t idx);

typedef uint32_t counter32_t;
typedef _Atomic(counter32_t) atomic_counter32_t;

typedef struct atomic_counters32_s
{
    atomic_counter32_t *container;
    size_t size;
} atomic_counters32_t;

void atomic_counters32_init(atomic_counters32_t* self, size_t new_size);

void atomic_counters32_deinit(atomic_counters32_t *self);

void atomic_counters32_add(atomic_counters32_t* self, size_t idx, int val);

counter32_t atomic_counters32_get(const atomic_counters32_t* self, size_t idx);

/**
 * Manual Reference Counter (MRC) for the unique table nodes.
 * Used for tracking dead nodes during dynamic variable reordering and
 * performing selective garbage collection.
 */
typedef struct mrc_s
{
    roaring_bitmap_t*       node_ids;       // indices of the nodes unique table
    _Atomic(size_t)         nnodes;         // number of all nodes in DD
    atomic_counters32_t     ref_nodes;      // number of internal references per node
    atomic_counters32_t     var_nnodes;     // number of nodes per variable
    atomic_bitmap_t         ext_ref_nodes;  // nodes with external references
} mrc_t;

/**
 * init/ deinit functions.
 */
void mrc_init(mrc_t* self, size_t nvars, size_t nnodes);

void mrc_deinit(mrc_t* self);

/**
 * setters
 */
void mrc_nnodes_set(mrc_t* self, int val);

/**
 * adders
 */
void mrc_ref_nodes_add(mrc_t* self, size_t idx, int val);

void mrc_var_nnodes_add(mrc_t* self, size_t idx, int val);

void mrc_nnodes_add(mrc_t* self, int val);

/**
 * getters
 */
counter16_t mrc_ext_ref_nodes_get(const mrc_t* self, size_t idx);

counter32_t mrc_ref_nodes_get(const mrc_t* self, size_t idx);

counter32_t mrc_var_nnodes_get(const mrc_t* self, size_t idx);

size_t mrc_nnodes_get(const mrc_t* self);

/**
 * @brief Perform selective garbage collection.
 *
 * @details This function performs selective garbage collection on the unique table nodes.
 * For every node with <node>.ref_count == 0 perform delete and decrease ref count of its children.
 * If the children become dead, delete them as well, repeat until no more dead nodes exist.
 */
#define mrc_gc(...) RUN(mrc_gc, __VA_ARGS__)
VOID_TASK_DECL_2(mrc_gc, mrc_t*, roaring_bitmap_t*)

int mrc_is_node_dead(const mrc_t* self, size_t idx);

void mrc_delete_node(mrc_t *self, size_t index, roaring_bitmap_t *old_ids);

/**
 * @brief Create a new node in the unique table. (with <add_id> == 1, not thread-safe!)
 */
MTBDD mrc_make_node(mrc_t *self, BDDVAR var, MTBDD low, MTBDD high, int* created, int add_id);

/**
 * @brief Create a new mapnode in the unique table. (with <add_id> == 1, not thread-safe!)
 */
MTBDD mrc_make_mapnode(mrc_t *self, BDDVAR var, MTBDD low, MTBDD high, int *created, int add_id);

#define mrc_collect_node_ids(...) CALL(mrc_collect_node_ids, __VA_ARGS__)
VOID_TASK_DECL_2(mrc_collect_node_ids, mrc_t*, llmsset_t)


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif //SYLVAN_BENCHMARKS_SYLVAN_MRC_H

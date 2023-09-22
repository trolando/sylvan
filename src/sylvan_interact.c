#include <sylvan_int.h>
#include <sylvan_align.h>

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>


void interact_deinit(interact_t *self)
{
    atomic_bitmap_deinit(self);
}

static inline size_t interact_get_nrows(const interact_t *self)
{
    double nrows = sqrt(self->size);
    return nrows < 0 ? 0 : (size_t) nrows;
}

inline void interact_set(interact_t *self, size_t row, size_t col)
{
    atomic_bitmap_set(self, (row * interact_get_nrows(self)) + col, memory_order_seq_cst);
}

inline int interact_get(const interact_t *self, size_t row, size_t col)
{
    return atomic_bitmap_get(self, (row * interact_get_nrows(self)) + col, memory_order_relaxed);
}

inline int interact_test(const interact_t *self, uint32_t x, uint32_t y)
{
    // ensure x < y
    // this is because we only keep the upper triangle of the matrix
    if (x > y) {
        int tmp = x;
        x = y;
        y = tmp;
    }
    return interact_get(self, x, y);
}

void interact_update(interact_t *self, atomic_bitmap_t *bitmap)
{
    size_t i, j;
    size_t nrows = interact_get_nrows(self);
    size_t ncols = nrows;
    for (i = 0; i < nrows - 1; i++) {
        if (atomic_bitmap_get(bitmap, i, memory_order_relaxed) == 1) {
            atomic_bitmap_clear(bitmap, i, memory_order_relaxed);
            for (j = i + 1; j < ncols; j++) {
                if (atomic_bitmap_get(bitmap, j, memory_order_relaxed) == 1) {
                    interact_set(self, i, j);
                }
            }
        }
    }
    atomic_bitmap_clear(bitmap, nrows - 1, memory_order_relaxed);
}

void interact_print(const interact_t *self)
{
    size_t nrows = interact_get_nrows(self);
    size_t ncols = nrows;
    printf("Interaction matrix: \n");
    printf("  \t");
    for (size_t i = 0; i < nrows; ++i) printf("%zu ", i);
    printf("\n");

    for (size_t i = 0; i < nrows; ++i) {
        printf("%zu \t", i);
        for (size_t j = 0; j < ncols; ++j) {
            printf("%d ", interact_test(self, i, j));
            if (j > 9) printf(" ");
            if (j > 99) printf(" ");
            if (j > 999) printf(" ");
        }
        printf("\n");
    }

    printf("\n");
}

/**
 *
 * @brief Find the support of f. (parallel)
 *
 * @sideeffect Accumulates in support the variables on which f depends.
 *
 * If F00 = F01 and F10 = F11, then F does not depend on <y>. If this is the case
 * for all the nodes of variable <x>, we say that variables <x> and <y> do not interact.
 *
 * Performs a tree search on the BDD to accumulate the support array of the variables on which f depends.
 *
 *        (x)F
 *       /   \
 *    (y)F0   (y)F1
 *    / \     / \
 *  F00 F01 F10 F11
 */
#define find_support(f, lvl_db, support, global, local) RUN(find_support, f, support, global, local)
VOID_TASK_5(find_support, MTBDD, f, levels_t*, lvl_db, atomic_bitmap_t*, support, atomic_bitmap_t*, global,
            atomic_bitmap_t*, local)
{
    uint64_t index = f & SYLVAN_TABLE_MASK_INDEX;
    if (index == 0 || index == 1 || index == sylvan_invalid) return;
    if (f == mtbdd_true || f == mtbdd_false) return;

    if (atomic_bitmap_get(local, index, memory_order_relaxed)) return;

    BDDVAR var = mtbdd_getvar(f);
    // set support bitmap, <var> contributes to the outcome of <f>
    atomic_bitmap_set(support, lvl_db->level_to_order[var], memory_order_relaxed);

    if (!mtbdd_isleaf(f)) {
        // visit all nodes reachable from <f>
        MTBDD f1 = mtbdd_gethigh(f);
        MTBDD f0 = mtbdd_getlow(f);
        CALL(find_support, f1, lvl_db, support, global, local);
        CALL(find_support, f0, lvl_db, support, global, local);
    }

    // locally visited node used to avoid duplicate node visit for a given tree
    atomic_bitmap_set(local, index, memory_order_relaxed);
    // globally visited node used to determining root nodes
    atomic_bitmap_set(global, index, memory_order_relaxed);
}

VOID_TASK_IMPL_5(interact_init, interact_t*, self, levels_t*, lvl_db, mrc_t*, mrc, size_t, nvars, size_t, nnodes)
{
    atomic_bitmap_init(self, nvars * nvars);

    atomic_bitmap_t support = (atomic_bitmap_t) {
            .container = NULL,
            .size = 0
    }; // support bitmap
    atomic_bitmap_t global = (atomic_bitmap_t) {
            .container = NULL,
            .size = 0
    }; // globally visited nodes bitmap (forest wise)
    atomic_bitmap_t local = (atomic_bitmap_t) {
            .container = NULL,
            .size = 0
    }; // locally visited nodes bitmap (tree wise)

    atomic_bitmap_init(&support, nvars);
    atomic_bitmap_init(&global, nnodes);
    atomic_bitmap_init(&local, nnodes);

    // start the tree traversals only form nodes with external references
    for (size_t index = atomic_bitmap_first(&mrc->ext_ref_nodes);
         index < nodes->table_size; index = atomic_bitmap_next(&mrc->ext_ref_nodes, index)) {
        // A node is a root of the DAG if it cannot be reached by nodes above it.
        // If a node was never reached during the previous searches,
        // then it is a root, and we start a new search from it.
        mtbddnode_t node = MTBDD_GETNODE(index);
        if (mtbddnode_isleaf(node)) {
            // if the node was a leaf, job done
            continue;
        }

        if (atomic_bitmap_get(&global, index, memory_order_relaxed) == 1) {
            // already visited node, thus can not be a root and we can skip it
            continue;
        }

        // visit all nodes reachable from <f>
        MTBDD f1 = mtbddnode_gethigh(node);
        MTBDD f0 = mtbddnode_getlow(node);
        CALL(find_support, f1, lvl_db, &support, &global, &local);
        CALL(find_support, f0, lvl_db, &support, &global, &local);

        BDDVAR var = mtbddnode_getvariable(node);
        // set support bitmap, <var> contributes to the outcome of <f>
        atomic_bitmap_set(&support, lvl_db->level_to_order[var], memory_order_relaxed);

        // clear locally visited nodes bitmap,
        atomic_bitmap_clear_all(&local);
        // update interaction matrix
        interact_update(self, &support);
    }

    atomic_bitmap_deinit(&support);
    atomic_bitmap_deinit(&global);
    atomic_bitmap_deinit(&local);
}
#include <sylvan_int.h>
#include <sylvan_align.h>

#include <errno.h>

VOID_TASK_DECL_4(mrc_collect_node_ids_par, uint64_t, uint64_t, atomic_bitmap_t*, roaring_bitmap_t*)

TASK_DECL_3(size_t, mrc_delete_node, mrc_t*, size_t, roaring_bitmap_t*)

TASK_DECL_5(size_t, mrc_gc_go, mrc_t*, uint64_t, uint64_t, roaring_bitmap_t*, roaring_bitmap_t*)

/**
 * Atomic counters
 */
void atomic_counters16_init(atomic_counters16_t *self, size_t new_size)
{
    atomic_counters16_deinit(self);
    self->container = (atomic_counter16_t *) alloc_aligned(sizeof(atomic_counter16_t[new_size]));
    if (self->container == NULL) {
        fprintf(stderr, "atomic_counter_realloc: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }
    self->size = new_size;
}

void atomic_counters16_deinit(atomic_counters16_t *self)
{
    if (self->container != NULL && self->size > 0) {
        free_aligned(self->container, self->size);
    }
    self->size = 0;
    self->container = NULL;
}

void atomic_counters16_add(atomic_counters16_t *self, size_t idx, int val)
{
    counter16_t curr = atomic_counters16_get(self, idx);
    if (curr == 0 && val < 0) return;
    if ((curr + val) >= COUNTER16_T_MAX) return;
    if (idx >= self->size) return;
    atomic_counter16_t *ptr = self->container + idx;
    atomic_fetch_add_explicit(ptr, val, memory_order_relaxed);
}

counter16_t atomic_counters16_get(const atomic_counters16_t *self, size_t idx)
{
    atomic_counter16_t *ptr = self->container + idx;
    return atomic_load_explicit(ptr, memory_order_relaxed);
}

void atomic_counters32_init(atomic_counters32_t *self, size_t new_size)
{
    atomic_counters32_deinit(self);
    self->container = (atomic_counter32_t *) alloc_aligned(sizeof(atomic_counter32_t[new_size]));
    if (self->container == NULL) {
        fprintf(stderr, "atomic_counter_realloc: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }
    self->size = new_size;
}

void atomic_counters32_deinit(atomic_counters32_t *self)
{
    if (self->container != NULL && self->size > 0) {
        free_aligned(self->container, self->size);
    }
    self->size = 0;
    self->container = NULL;
}

void atomic_counters32_add(atomic_counters32_t *self, size_t idx, int val)
{
    counter32_t curr = atomic_counters32_get(self, idx);
    if (curr == 0 && val < 0) return;
    if ((curr + val) >= COUNTER32_T_MAX) return;
    if (idx >= self->size) return;
    atomic_counter32_t *ptr = self->container + idx;
    atomic_fetch_add_explicit(ptr, val, memory_order_relaxed);
}

counter32_t atomic_counters32_get(const atomic_counters32_t *self, size_t idx)
{
    atomic_counter32_t *ptr = self->container + idx;
    return atomic_load_explicit(ptr, memory_order_relaxed);
}

/**
 * @brief MRC initialization.
 *
 * @details Traverse the forest and count the number of nodes and variables and their internal and external references.
 *
 * @preconditions
 * - The forest must be initialized.
 */
void mrc_init(mrc_t *self, size_t nvars, size_t nnodes)
{
    // memory usage: # of nodes * sizeof (counter_t) bits       (16n)
    atomic_counters32_init(&self->ref_nodes, nnodes);
    // memory usage: # of variables * sizeof (counter_t) bits   (16v)
    atomic_counters32_init(&self->var_nnodes, nvars);
    // memory usage: # of nodes * 1 bit                         (n)
    atomic_bitmap_init(&self->ext_ref_nodes, nnodes);

    mrc_nnodes_set(self, 2);

    roaring_uint32_iterator_t it;
    roaring_init_iterator(self->node_ids, &it);
    roaring_move_uint32_iterator_equalorlarger(&it, 2);

    while (it.has_value) {
        size_t index = it.current_value;
        roaring_advance_uint32_iterator(&it);
        if (index == 0 || index == 1) continue;
        mrc_nnodes_add(self, 1);

        mtbddnode_t node = MTBDD_GETNODE(index);
        BDDVAR var = mtbddnode_getvariable(node);
        mrc_var_nnodes_add(self, var, 1);

        if (mtbddnode_isleaf(node)) continue;

        MTBDD f1 = mtbddnode_gethigh(node);
        size_t f1_index = f1 & SYLVAN_TABLE_MASK_INDEX;
        if (f1 != sylvan_invalid && (f1_index) != 0 && (f1_index) != 1) {
            mrc_ref_nodes_add(self, f1_index, 1);
        }

        MTBDD f0 = mtbddnode_getlow(node);
        size_t f0_index = f0 & SYLVAN_TABLE_MASK_INDEX;
        if (f0 != sylvan_invalid && (f0_index) != 0 && (f0_index) != 1) {
            mrc_ref_nodes_add(self, f0_index, 1);
        }
    }

    roaring_init_iterator(self->node_ids, &it);
    roaring_move_uint32_iterator_equalorlarger(&it, 2);

    mtbdd_re_mark_external_refs(self->ext_ref_nodes.container);
    mtbdd_re_mark_protected(self->ext_ref_nodes.container);
}

void mrc_deinit(mrc_t *self)
{
    if (self->node_ids == NULL) roaring_bitmap_free(self->node_ids);
    atomic_counters32_deinit(&self->ref_nodes);
    atomic_counters32_deinit(&self->var_nnodes);
    atomic_bitmap_deinit(&self->ext_ref_nodes);
}

void mrc_nnodes_set(mrc_t *self, int val)
{
    atomic_store_explicit(&self->nnodes, val, memory_order_relaxed);
}

void mrc_ref_nodes_add(mrc_t *self, size_t idx, int val)
{
    atomic_counters32_add(&self->ref_nodes, idx, val);
}

void mrc_var_nnodes_add(mrc_t *self, size_t idx, int val)
{
    atomic_counters32_add(&self->var_nnodes, idx, val);
}

void mrc_nnodes_add(mrc_t *self, int val)
{
    size_t curr = mrc_nnodes_get(self);
    if (curr == 0 && val < 0) return;
    atomic_fetch_add_explicit(&self->nnodes, val, memory_order_relaxed);
}

counter16_t mrc_ext_ref_nodes_get(const mrc_t *self, size_t idx)
{
    return atomic_bitmap_get(&self->ext_ref_nodes, idx, memory_order_relaxed);
}

counter32_t mrc_ref_nodes_get(const mrc_t *self, size_t idx)
{
    return atomic_counters32_get(&self->ref_nodes, idx);
}

counter32_t mrc_var_nnodes_get(const mrc_t *self, size_t idx)
{
    return atomic_counters32_get(&self->var_nnodes, idx);
}

size_t mrc_nnodes_get(const mrc_t *self)
{
    return atomic_load_explicit(&self->nnodes, memory_order_relaxed);
}

int mrc_is_node_dead(const mrc_t *self, size_t idx)
{
    counter16_t int_count = mrc_ref_nodes_get(self, idx);
    if (int_count > 0) return 0;
    // mrc_ext_ref_nodes_get is an atomic bitmap call which is much more expensive than mrc_ref_nodes_get
    // thus, invoke it only if really necessary
    counter16_t ext_count = mrc_ext_ref_nodes_get(self, idx);
    if (ext_count > 0) return 0;
    return llmsset_is_marked(nodes, idx) == 1;
}

VOID_TASK_IMPL_2(mrc_gc, mrc_t*, self, roaring_bitmap_t*, ids)
{
    roaring_bitmap_t dead_ids;
    roaring_bitmap_init_with_capacity(&dead_ids, nodes->table_size);

    size_t deleted_nnodes = CALL(mrc_gc_go, self, 0, nodes->table_size, &dead_ids, ids);
    if (deleted_nnodes == 0) return;

    // calling bitmap remove per each node is more expensive than calling it once with many ids
    // thus, we group the ids into <arr> and let the bitmap delete them in one go
    roaring_uint32_iterator_t it_old;
    roaring_init_iterator(&dead_ids, &it_old);
    uint32_t arr[deleted_nnodes];
    size_t x = 0;
    while (it_old.has_value) {
        arr[x] = it_old.current_value;
        roaring_advance_uint32_iterator(&it_old);
        x++;
    }
    roaring_bitmap_remove_many(self->node_ids, deleted_nnodes, arr);

#if SYLVAN_USE_LINEAR_PROBING
    sylvan_clear_and_mark();
    sylvan_rehash_all();
#else
    CALL(llmsset_reset_all_regions);
#endif
}

#define index(x) ((x) & SYLVAN_TABLE_MASK_INDEX)

TASK_IMPL_5(size_t, mrc_gc_go, mrc_t*, self, uint64_t, first, uint64_t, count, roaring_bitmap_t *, dead_ids,
            roaring_bitmap_t *, ids)
{
    roaring_uint32_iterator_t it;
    roaring_init_iterator(ids, &it);
    if (!roaring_move_uint32_iterator_equalorlarger(&it, first)) return 0;

    size_t deleted = 0;

    const size_t end = first + count;
    while (it.has_value && it.current_value < end) {
        if (mrc_is_node_dead(self, it.current_value)) {
            deleted += CALL(mrc_delete_node, self, it.current_value, dead_ids);
        }
        roaring_advance_uint32_iterator(&it);
    }
    if (deleted > 0) mrc_nnodes_add(self, -(int) deleted);
    return deleted;
}

TASK_IMPL_3(size_t, mrc_delete_node, mrc_t*, self, size_t, index, roaring_bitmap_t*, dead_ids)
{
    size_t deleted = 1;
    mtbddnode_t f = MTBDD_GETNODE(index);
    // roaring_bitmap_add does not allow concurrent writes, thus we invoke recursive mrc_delete_node function sequentially
    roaring_bitmap_add(dead_ids, index);
    mrc_var_nnodes_add(self, mtbddnode_getvariable(f), -1);

    if (!mtbddnode_isleaf(f)) {
        MTBDD f1 = mtbddnode_gethigh(f);
        if (f1 != sylvan_invalid && index(f1) != 0 && index(f1) != 1) {
            mrc_ref_nodes_add(&reorder_db->mrc, index(f1), -1);
            if (mrc_is_node_dead(self, index(f1))) {
                deleted += CALL(mrc_delete_node, self, index(f1), dead_ids);
            }
        }
        MTBDD f0 = mtbddnode_getlow(f);
        if (f0 != sylvan_invalid && index(f0) != 0 && index(f0) != 1) {
            mrc_ref_nodes_add(&reorder_db->mrc, index(f0), -1);
            if (mrc_is_node_dead(self, index(f0))) {
                deleted += CALL(mrc_delete_node, self, index(f0), dead_ids);
            }
        }
    }
#if !SYLVAN_USE_LINEAR_PROBING
    llmsset_clear_one_hash(nodes, index);
    llmsset_clear_one_data(nodes, index);
#endif
    return deleted;
}

VOID_TASK_IMPL_2(mrc_collect_node_ids, mrc_t*, self, llmsset_t, dbs)
{
    atomic_bitmap_t bitmap = {
            .container = dbs->bitmap2,
            .size = dbs->table_size
    };
    roaring_bitmap_clear(self->node_ids);
    roaring_bitmap_init_with_capacity(self->node_ids, llmsset_count_marked(dbs));
    CALL(mrc_collect_node_ids_par, 0, dbs->table_size, &bitmap, self->node_ids);
}

VOID_TASK_IMPL_4(mrc_collect_node_ids_par, uint64_t, first, uint64_t, count, atomic_bitmap_t*, bitmap,
                 roaring_bitmap_t *, collected_ids)
{
    if (count > 1024) {
        // standard reduction pattern with local roaring bitmaps collecting new node indices
        size_t split = count / 2;
        roaring_bitmap_t a;
        roaring_bitmap_init_cleared(&a);
        SPAWN(mrc_collect_node_ids_par, first, split, bitmap, &a);
        roaring_bitmap_t b;
        roaring_bitmap_init_cleared(&b);
        CALL(mrc_collect_node_ids_par, first + split, count - split, bitmap, &b);
        roaring_bitmap_or_inplace(collected_ids, &b);
        SYNC(mrc_collect_node_ids_par);
        roaring_bitmap_or_inplace(collected_ids, &a);
        roaring_bitmap_clear(&a);
        roaring_bitmap_clear(&b);
        return;
    }
    // skip buckets 0 and 1
    if (first < 2) {
        count = count + first - 2;
        first = 2;
    }

    const size_t end = first + count;
    for (first = atomic_bitmap_next(bitmap, first - 1); first < end; first = atomic_bitmap_next(bitmap, first)) {
        roaring_bitmap_add(collected_ids, first);
    }
}

MTBDD mrc_make_node(mrc_t *self, BDDVAR var, MTBDD low, MTBDD high, int *created, int add_id)
{
    MTBDD new = mtbdd_varswap_makenode(var, low, high, created);
    if (new == mtbdd_invalid) {
        return mtbdd_invalid;
    }
    if (*created) {
        mrc_nnodes_add(self, 1);
        mrc_var_nnodes_add(self, var, 1);
        if (add_id) roaring_bitmap_add(self->node_ids, index(new));
        mrc_ref_nodes_add(self, index(new), 1);
        mrc_ref_nodes_add(self, index(high), 1);
        mrc_ref_nodes_add(self, index(low), 1);
    } else {
        mrc_ref_nodes_add(self, index(new), 1);
    }
    return new;
}

MTBDD mrc_make_mapnode(mrc_t *self, BDDVAR var, MTBDD low, MTBDD high, int *created, int add_id)
{
    MTBDD new = mtbdd_varswap_makemapnode(var, low, high, created);
    if (new == mtbdd_invalid) {
        return mtbdd_invalid;
    }
    if (*created) {
        mrc_nnodes_add(self, 1);
        mrc_var_nnodes_add(self, var, 1);
        if (add_id) roaring_bitmap_add(self->node_ids, index(new));
        mrc_ref_nodes_add(self, index(new), 1);
        mrc_ref_nodes_add(self, index(high), 1);
        mrc_ref_nodes_add(self, index(low), 1);
    } else {
        mrc_ref_nodes_add(self, index(new), 1);
    }
    return new;
}
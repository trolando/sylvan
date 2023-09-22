#include <sylvan_int.h>
#include <sylvan_align.h>
#include <errno.h>      // for errno

static size_t levels_size = 0; // size of the arrays in levels_t used to realloc memory

size_t levels_get(levels_t* self, uint64_t level)
{
    return self->table[self->level_to_order[level]];
}

size_t levels_get_count(levels_t* self)
{
    return self->count;
}

uint64_t levels_new_one(levels_t* self)
{
    levels_new_many(1);
    return self->table[levels_get_count(self) - 1];
}

int levels_new_many(size_t amount)
{
    if (reorder_db->levels.count + amount >= levels_size) {
        // just round up to the next multiple of 64 value
        // probably better than doubling anyhow...
        levels_size = (reorder_db->levels.count + amount + 63) & (~63LL);
        reorder_db->levels.table = (_Atomic (uint64_t) *) realloc(reorder_db->levels.table, sizeof(uint64_t[levels_size]));
        reorder_db->levels.level_to_order = (_Atomic (uint32_t) *) realloc(reorder_db->levels.level_to_order, sizeof(uint32_t[levels_size]));
        reorder_db->levels.order_to_level = (_Atomic (uint32_t) *) realloc(reorder_db->levels.order_to_level, sizeof(uint32_t[levels_size]));

        if (reorder_db->levels.table == NULL || reorder_db->levels.level_to_order == NULL || reorder_db->levels.order_to_level == NULL) {
            fprintf(stderr, "levels_new_many failed to realloc new memory: %s!\n", strerror(errno));
            exit(1);
        }
    }
    for (size_t i = 0; i < amount; i++) {
        reorder_db->levels.table[reorder_db->levels.count] = sylvan_invalid;
        reorder_db->levels.level_to_order[reorder_db->levels.count] = reorder_db->levels.count;
        reorder_db->levels.order_to_level[reorder_db->levels.count] = reorder_db->levels.count;
        reorder_db->levels.count++;
    }
    return 1;
}

uint64_t levels_new_node(levels_t* self, uint32_t level, uint64_t low, uint64_t high)
{
    if (level >= self->count) {
        fprintf(stderr, "mtbdd_levels_makenode failed. Out of level bounds.");
        return 0;
    }

    BDDVAR order = self->level_to_order[level];
    self->table[order] = mtbdd_makenode(order, low, high);

    return self->table[order];
}

void levels_reset(levels_t* self)
{
    if (levels_size != 0) {
        if (!self->table) free(self->table);
        self->table = NULL;

        if (!self->level_to_order) free(self->level_to_order);
        self->level_to_order = NULL;

        if (!self->order_to_level) free(self->order_to_level);
        self->order_to_level = NULL;

        self->count = 0;
        levels_size = 0;
    }
}

uint64_t levels_ithlevel(levels_t* self, uint32_t level)
{
    if (level < self->count) {
        if (levels_get(self, level) == sylvan_invalid){
            levels_new_node(self, level, mtbdd_false, mtbdd_true);
        }
        if (!llmsset_is_marked(nodes, levels_get(self, level) & SYLVAN_TABLE_MASK_INDEX)) {
            levels_new_node(self, level, mtbdd_false, mtbdd_true);
        }
    } else {
        size_t amount = level - self->count + 1;
        levels_new_many(amount);
        levels_new_node(self, level, mtbdd_false, mtbdd_true);
    }

    return levels_get(self, level);
}

uint32_t levels_order_to_level(levels_t *self, uint32_t var)
{
    if (var < self->count) return self->order_to_level[var];
    else return var;
}

uint32_t
levels_level_to_order(levels_t *self, uint32_t level)
{
    if (level < self->count) return self->level_to_order[level];
    else return level;
}

uint32_t levels_swap(levels_t *self, uint32_t x, uint32_t y)
{
    if (x >= self->count || y >= self->count) return 0;

    self->order_to_level[self->level_to_order[x]] = y;
    self->order_to_level[self->level_to_order[y]] = x;

    uint32_t tmp = self->level_to_order[x];

    self->level_to_order[x] = self->level_to_order[y];
    self->level_to_order[y] = tmp;

    return 1;
}


/**
 * This function is called during garbage collection and
 * marks all managed level BDDs so they are kept.
 */
VOID_TASK_0(mtbdd_gc_mark_managed_refs)
{
    for (size_t i = 0; i < reorder_db->levels.count; i++) {
        if (reorder_db->levels.table[i] != sylvan_invalid){
            llmsset_mark(nodes, MTBDD_STRIPMARK(reorder_db->levels.table[i]));
        }
    }
}

void levels_gc_add_mark_managed_refs(void)
{
    sylvan_gc_add_mark(TASK(mtbdd_gc_mark_managed_refs));
}

/**
 * Sort level counts using gnome sort.
 */
void levels_gnome_sort(levels_t *self, int *levels_arr, const size_t *level_counts)
{
    unsigned int i = 1;
    unsigned int j = 2;
    while (i < self->count) {
        long p = levels_arr[i - 1] == -1 ? -1 : (long) level_counts[self->level_to_order[levels_arr[i - 1]]];
        long q = levels_arr[i] == -1 ? -1 : (long) level_counts[self->level_to_order[levels_arr[i]]];
        if (p < q) {
            int t = levels_arr[i];
            levels_arr[i] = levels_arr[i - 1];
            levels_arr[i - 1] = t;
            if (--i) continue;
        }
        i = j++;
    }
}

// set levels below the threshold to -1
void levels_mark_threshold(levels_t *self, int *level, const size_t *level_counts, uint32_t threshold)
{
    for (unsigned int i = 0; i < self->count; i++) {
        if (level_counts[self->level_to_order[i]] < threshold) level[i] = -1;
        else level[i] = i;
    }
}
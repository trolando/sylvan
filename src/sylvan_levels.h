#ifndef SYLVAN_SYLVAN_LEVELS_H
#define SYLVAN_SYLVAN_LEVELS_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * When using dynamic variable reordering, it is strongly recommended to use
 * "levels" instead of working directly with the internal variables.
 *
 * Dynamic variable reordering requires that variables are consecutive.
 * Initially, variables are assigned linearly, starting with 0.
 */
typedef struct levels_s {
    _Atomic(uint64_t)*      table;                   // array holding the 1-node BDD for each level
    size_t                  count;                   // number of created levels
    _Atomic(uint32_t)*      level_to_order;          // current level wise var permutation (level to variable label)
    _Atomic(uint32_t)*      order_to_level;          // current variable wise level permutation (variable label to level)
} levels_t;


/**
 * @brief Get the number of levels
 */
size_t levels_get_count(levels_t* self);

/**
 * @brief Create the next level and return the BDD representing the variable (ithlevel)
 * @details The BDDs representing managed levels are always kept during garbage collection.
 * NOTE: not currently thread-safe.
 */
uint64_t levels_new_one(levels_t* self);

/**
 * @brief Create the next <amount> levels
 * @details The BDDs representing managed levels are always kept during garbage collection. Not currently thread-safe.
 */
int levels_new_many(size_t amount);

/**
 * @brief Insert a node at given level with given low and high nodes
 */
uint64_t levels_new_node(levels_t* self, uint32_t level, uint64_t low, uint64_t high);

/**
 * \brief  Reset all levels.
 */
void levels_reset(levels_t* self);


/**
 * \brief  Get the BDD node representing "if level then true else false"
 * \details  Order a node does not change after a swap, meaning it is in the same level,
 * however, after a swap they can point to a different variable
 * \param level for which the BDD needs to be returned
 */
uint64_t levels_ithlevel(levels_t* self, uint32_t level);

/**
 * @brief  Get the level of the given variable
 */
uint32_t levels_order_to_level(levels_t* self, uint32_t var);

/**
 * @brief  Get the variable of the given level
 */
uint32_t levels_level_to_order(levels_t* self, uint32_t level);

uint32_t levels_swap(levels_t *self, uint32_t x, uint32_t y);

/**
 * \brief  Add callback to mark managed references during garbage collection.
 * \details This is used for the dynamic variable reordering.
 */
void levels_gc_add_mark_managed_refs(void);

/**
 * @brief  Mark each level_count -1 which is below the threshold.
 */
void levels_mark_threshold(levels_t* self, int* level, const size_t* level_counts, uint32_t threshold);

/**
 * @brief  Sort the levels in descending order according to the number of nodes.
 */
void levels_gnome_sort(levels_t* self, int *levels, const size_t* level_counts);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif //SYLVAN_SYLVAN_LEVELS_H

#ifndef SYLVAN_VAR_REORDER_H
#define SYLVAN_VAR_REORDER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief Callback type
 */
LACE_TYPEDEF_CB(void, re_hook_cb);

/**
  @brief Type of reordering algorithm.
*/
typedef enum {
    SYLVAN_REORDER_SIFT,
    SYLVAN_REORDER_BOUNDED_SIFT,
} reordering_type_t;

typedef int (*re_term_cb)();

typedef enum reorder_result {
    /// the operation was aborted and rolled back
    SYLVAN_REORDER_ROLLBACK = 1,
    /// success
    SYLVAN_REORDER_SUCCESS = 0,
    //// cannot clear in phase 0, no marked nodes remaining
    SYLVAN_REORDER_P0_CLEAR_FAIL = -1,
    //// cannot rehash in phase 1, no marked nodes remaining
    SYLVAN_REORDER_P1_REHASH_FAIL = -2,
    /// cannot rehash in phase 1, and marked nodes remaining
    SYLVAN_REORDER_P1_REHASH_FAIL_MARKED = -3,
    /// cannot rehash in phase 2, no marked nodes remaining
    SYLVAN_REORDER_P2_REHASH_FAIL = -4,
    /// cannot create node in phase 2 (ergo marked nodes remaining)
    SYLVAN_REORDER_P2_CREATE_FAIL = -5,
    /// cannot create mapnode in phase 2 (ergo marked nodes remaining)
    SYLVAN_REORDER_P2_MAPNODE_CREATE_FAIL = -6,
    /// cannot rehash and cannot create node in phase 2
    SYLVAN_REORDER_P2_REHASH_AND_CREATE_FAIL = -7,
    //// cannot rehash in phase 3, maybe there are marked nodes remaining
    SYLVAN_REORDER_P3_REHASH_FAIL = -8,
    //// cannot clear in phase 3, maybe there are marked nodes remaining
    SYLVAN_REORDER_P3_CLEAR_FAIL = -9,
    /// the operation failed fast because there are no registered variables
    SYLVAN_REORDER_NO_REGISTERED_VARS = -10,
    /// the operation failed fast because the varswap was not initialised
    SYLVAN_REORDER_NOT_INITIALISED = -11,
    /// the operation failed fast because the varswap was already running
    SYLVAN_REORDER_ALREADY_RUNNING = -12,
    /// the operation did not even start because there was not enough memory
    SYLVAN_REORDER_NOT_ENOUGH_MEMORY = -13,
} reorder_result_t;

/**
 * @brief Provide description for given result.
 *
 * @details Requires buffer with length at least equal to 100
 *
 * @param tag
 * @param result based on which the description is determined
 * @param buf buffer into which the description will be copied
 * @param buf_len
 */
void sylvan_reorder_resdescription(reorder_result_t result, char *buf, size_t buf_len);

/**
 * @brief Add a hook that is called before dynamic variable reordering begins.
 */
void sylvan_re_hook_prere(re_hook_cb callback);

/**
 * @brief Add a hook that is called after dynamic variable reordering is finished.
 */
void sylvan_re_hook_postre(re_hook_cb callback);

/**
 * @brief Add a hook that is called after dynamic variable reordering managed to reduce number of nodes.
 */
void sylvan_re_hook_progre(re_hook_cb callback);

/**
 * @brief Add a hook that is called regularly to see whether sifting should terminate.
 */
void sylvan_re_hook_termre(re_term_cb callback);

/**
 * @brief Initialize the dynamic variable reordering.
 */
void sylvan_init_reorder(void);

/**
 * @brief Quit the dynamic variable reordering.
 */
void sylvan_quit_reorder(void);


/**
 * @brief Set threshold for the number of nodes per level to consider during the reordering.
 * @details If the number of nodes per level is less than the threshold, the level is skipped during the reordering.
 * @param threshold The threshold for the number of nodes per level.
*/
void sylvan_set_reorder_nodes_threshold(uint32_t threshold);

/**
 * @brief Set the maximum growth coefficient.
 * @details The maximum growth coefficient is used to calculate the maximum growth of the number of nodes during the reordering.
 *        If the number of nodes grows more than the maximum growth coefficient , sift up/down is terminated.
 * @param max_growth The maximum growth coefficient.
*/
void sylvan_set_reorder_maxgrowth(float max_growth);

/**
 * @brief Set the maximum number of swaps per sifting.
 * @param max_swap The maximum number of swaps per sifting.
*/
void sylvan_set_reorder_maxswap(uint32_t max_swap);

/**
 * @brief Set the maximum number of vars swapped per sifting.
 * @param max_var The maximum number of vars swapped per sifting.
 */
void sylvan_set_reorder_maxvar(uint32_t max_var);

/**
 * @brief Set the time limit in minutes for the reordering.
 * @param time_limit The time limit for the reordering.
 */
void sylvan_set_reorder_timelimit_min(double time_limit);

/**
 * @brief Set the time limit in seconds for the reordering.
 * @param time_limit The time limit for the reordering.
 */
void sylvan_set_reorder_timelimit_sec(double time_limit);

/**
 * @brief Set the time limit in milliseconds for the reordering.
 * @param time_limit The time limit for the reordering.
 */
void sylvan_set_reorder_timelimit_ms(double time_limit);

/**
 * @brief Set the the flag to print the progress of the reordering.
 * @param verbose The flag to print the progress of the reordering.
 */
void sylvan_set_reorder_verbose(int is_verbose);

/**
 * @brief Set the the flag to print the progress of the reordering.
 * @param verbose The flag to print the progress of the reordering.
 */
void sylvan_set_reorder_type(reordering_type_t type);

void sylvan_set_reorder_print(bool is_on);

/**
 * @brief Reduce the heap size in the entire forest.
 *
 * @details Implementation of Rudell's sifting algorithm.
 * This function performs stop-the-world operation similar to garbage collection.
 * It proceeds as follows:
 * 1. Order all the variables according to the number of entries in each unique table.
 * 2. Sift the variable up and down, remembering each time the total size of the bdd size.
 * 3. Select the best permutation.
 * 4. Repeat 2 and 3 for all variables in given range.
 *
 * @sideeffect order of variables is changed, mappings level -> order and order -> level are updated
 */
void sylvan_reduce_heap(reordering_type_t type);

/**
 * @brief Maybe reduce the heap size in the entire forest.
 */
void sylvan_test_reduce_heap();

TASK_DECL_1(reorder_result_t, sylvan_reorder_perm, const uint32_t*);
/**
  @brief Reorder the variables in the BDDs according to the given permutation.

  @details The permutation is an array of BDD labels, where the i-th element is the label
  of the variable that should be moved to position i. The size
  of the array should be equal or greater to the number of variables
  currently in use.
 */
#define sylvan_reorder_perm(permutation)  RUN(sylvan_reorder_perm, permutation)

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif //SYLVAN_VAR_REORDER_H


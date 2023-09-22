#ifndef SYLVAN_VAR_REORDER_DB_H
#define SYLVAN_VAR_REORDER_DB_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct re_term_entry
{
    struct re_term_entry *next;
    re_term_cb cb;
} *re_term_entry_t;

typedef struct re_hook_entry
{
    struct re_hook_entry *next;
    re_hook_cb cb;
} *re_hook_entry_t;

typedef struct sifting_state
{
    uint32_t pos;
    int size;
    uint32_t best_pos;
    int best_size;
    uint32_t low;
    uint32_t high;
} sifting_state_t;

typedef struct reorder_config
{
    double                  t_start_sifting;      // start time of the sifting
    uint32_t                threshold;            // threshold for number of nodes per level
    double                  max_growth;           // coefficient used to calculate maximum growth
    uint32_t                max_swap;             // maximum number of swaps per sifting
    uint32_t                varswap_count;        // number of swaps completed
    uint32_t                max_var;              // maximum number of vars swapped per sifting
    uint32_t                total_num_var;        // number of vars sifted
    double                  time_limit_ms;        // time limit in milliseconds
    reordering_type_t       type;                 // type of reordering algorithm
    bool                    print_stat;           // flag to print the progress of the reordering
    size_t                  size_threshold;       // reorder if this size is reached
} reorder_config_t;

typedef struct reorder_db_s
{
    mrc_t                   mrc;                  // reference counters used for the unique table nodes
    interact_t              matrix;               // bitmap used for storing the square variable interaction matrix (use sylvan_interact with it)
    levels_t                levels;               // levels of the unique table nodes
    reorder_config_t        config;               // configuration for the sifting
    size_t                  call_count;           // number of reordering calls
    bool                    is_initialised;       // is dynamic reordering initialised
    bool                    is_reordering;        // is dynamic reordering in progress
} *reorder_db_t;

reorder_db_t reorder_db_init();

void reorder_db_deinit(reorder_db_t self);

static inline void reorder_set_default_config(reorder_config_t *configs)
{
    configs->threshold = SYLVAN_REORDER_NODES_THRESHOLD;
    configs->max_growth = SYLVAN_REORDER_GROWTH;
    configs->max_swap = SYLVAN_REORDER_MAX_SWAPS;
    configs->max_var = SYLVAN_REORDER_MAX_VAR;
    configs->time_limit_ms = SYLVAN_REORDER_TIME_LIMIT_MS;
    configs->type = SYLVAN_REORDER_TYPE_DEFAULT;
    configs->print_stat = SYLVAN_REORDER_PRINT_STAT;
    configs->size_threshold = SYLVAN_REORDER_SIZE_THRESHOLD;
}

static inline int sylvan_reorder_issuccess(reorder_result_t result)
{
    return result == SYLVAN_REORDER_SUCCESS ||
           result == SYLVAN_REORDER_NOT_INITIALISED ||
           result == SYLVAN_REORDER_ROLLBACK;
}

uint64_t get_nodes_count();


/**
 * @brief Sift given variable up from its current level to the target level.
 * @sideeffect order of variables is changed
 */
TASK_DECL_1(reorder_result_t, sylvan_siftdown, sifting_state_t*);
#define sylvan_siftdown(state) CALL(sylvan_siftdown, state)

/**
 * @brief Sift given variable down from its current level to the target level.
 * @sideeffect order of variables is changed
 */
TASK_DECL_1(reorder_result_t, sylvan_siftup, sifting_state_t*);
#define sylvan_siftup(state) CALL(sylvan_siftup, state)

/**
 * @brief Sift a variable to its best level.
 * @param pos - variable to sift
 * @param target_pos - target position (w.r.t. dynamic variable reordering)
 */
TASK_DECL_1(reorder_result_t, sylvan_siftback, sifting_state_t*);
#define sylvan_siftback(state) CALL(sylvan_siftback, state)

#define sylvan_pre_reorder(type) RUN(sylvan_pre_reorder, type)
VOID_TASK_DECL_1(sylvan_pre_reorder, reordering_type_t)

#define sylvan_post_reorder() RUN(sylvan_post_reorder)
VOID_TASK_DECL_0(sylvan_post_reorder)

void sylvan_reorder_resdescription(reorder_result_t result, char *buf, size_t buf_len);

void sylvan_print_reorder_res(reorder_result_t result);

void sylvan_reorder_type_description(reordering_type_t type, char *buf, size_t buf_len);

int should_terminate_sifting(const struct reorder_config *reorder_config);

int should_terminate_reordering(const struct reorder_config *reorder_config);

VOID_TASK_DECL_0(reorder_db_call_progress_hooks)

#define reorder_db_call_progress_hooks() CALL(reorder_db_call_progress_hooks)


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif //SYLVAN_VAR_REORDER_DB_H
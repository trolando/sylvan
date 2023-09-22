#ifndef SYLVAN_SYLVAN_VAR_SWAP_H
#define SYLVAN_SYLVAN_VAR_SWAP_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

 /**
  * @brief Swaps two consecutive variables in the entire forest.
  */
TASK_DECL_1(reorder_result_t, sylvan_varswap, uint32_t)
#define sylvan_varswap(p) CALL(sylvan_varswap, p)

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif //SYLVAN_SYLVAN_VAR_SWAP_H

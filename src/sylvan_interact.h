#ifndef SYLVAN_SYLVAN_INTERACT_H
#define SYLVAN_SYLVAN_INTERACT_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef atomic_bitmap_t interact_t;

VOID_TASK_DECL_5(interact_init, interact_t*, levels_t*, mrc_t*, size_t, size_t)
/**
 * @brief Initialize the variable interaction matrix.
 *
 * @details The interaction matrix is a bitmap of size n*n, where n is the number of variables.
 *
 * @memory: # of variables * # of variables * 1 bit -> O(v^2)
 */
#define interact_init(i, l, m, v, n) RUN(interact_init, i, l, m, v, n)

void interact_deinit(interact_t *self);

void interact_set(interact_t *self, size_t row, size_t col);

int interact_get(const interact_t *self, size_t row, size_t col);

int interact_test(const interact_t *self, uint32_t x, uint32_t y);

/**
  @brief Marks as interacting all pairs of variables that appear in
  support.

  @details If support[i] == support[j] == 1, sets the (i,j) entry
  of the interaction matrix to 1.

  @sideeffect Clears support.

*/
void interact_update(interact_t *self, atomic_bitmap_t *support);

void interact_print(const interact_t *self);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif //SYLVAN_SYLVAN_INTERACT_H

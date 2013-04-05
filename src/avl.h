#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * AVL implementation with height memoization
 *
 * Usage of this AVL implementation:
 * 
 * struct my_struct { ... };
 * AVL(some_name, my_struct)
 * {
 *    Compare struct my_struct *left with struct my_struct *right
 *    Return <0 when left<right, >0 when left>right, 0 when left=right
 * }
 *
 * You get: 
 *  - int some_name_search(avl_node_t root_node, struct my_struct *data);
 *  - void some_name_insert(avl_node_t *root_node, struct my_struct *data);
 *  - void some_name_free(avl_node_t *root_node);
 *
 * avl_node_t the_root = NULL;
 * struct mystuff;
 * if (!some_name_search(the_root, &mystuff)) some_name_insert(&the_root, &mystuff);
 * some_name_free(&the_root);
 */

typedef struct avl_node 
{ 
    struct avl_node *avl_left_child, *avl_right_child; 
    int avl_height;
    char pad[8-sizeof(int)];
    char data[0];
} *avl_node_t; 

static inline int avl_get_height(avl_node_t node)
{
    return node == NULL ? 0 : node->avl_height;
}

static inline void avl_update_height(avl_node_t node)
{
    int h1 = avl_get_height(node->avl_left_child);
    int h2 = avl_get_height(node->avl_right_child);
    node->avl_height = 1 + (h1 > h2 ? h1 : h2);
}

static inline int avl_update_height_get_balance(avl_node_t node)
{
    int h1 = avl_get_height(node->avl_left_child);
    int h2 = avl_get_height(node->avl_right_child);
    node->avl_height = 1 + (h1 > h2 ? h1 : h2);
    return h1 - h2;
}

static inline int avl_verify_height(avl_node_t node)
{
    int h1 = avl_get_height(node->avl_left_child);
    int h2 = avl_get_height(node->avl_right_child);
    int expected_height = 1 + (h1 > h2 ? h1 : h2);
    return expected_height == avl_get_height(node);
}

static inline int avl_check_consistent(avl_node_t root)
{
    if (root == NULL) return 1;
    if (!avl_check_consistent(root->avl_left_child)) return 0;
    if (!avl_check_consistent(root->avl_right_child)) return 0;
    if (!avl_verify_height(root)) return 0;
    return 1;
}

static avl_node_t avl_rotate_LL(avl_node_t parent) 
{ 
    avl_node_t child = parent->avl_left_child; 
    parent->avl_left_child = child->avl_right_child; 
    child->avl_right_child = parent; 
    avl_update_height(parent);
    avl_update_height(child);
    return child; 
} 

static avl_node_t avl_rotate_RR(avl_node_t parent) 
{ 
    avl_node_t child = parent->avl_right_child; 
    parent->avl_right_child = child->avl_left_child; 
    child->avl_left_child = parent; 
    avl_update_height(parent);
    avl_update_height(child);
    return child; 
} 

static avl_node_t avl_rotate_RL(avl_node_t parent) 
{ 
    avl_node_t child = parent->avl_right_child; 
    parent->avl_right_child = avl_rotate_LL(child); 
    return avl_rotate_RR(parent); 
} 

static avl_node_t avl_rotate_LR(avl_node_t parent) 
{ 
    avl_node_t child = parent->avl_left_child; 
    parent->avl_left_child = avl_rotate_RR(child); 
    return avl_rotate_LL(parent); 
} 

static inline int avl_get_balance(avl_node_t node) 
{ 
    if (node == NULL) return 0; 
    return avl_get_height(node->avl_left_child) - avl_get_height(node->avl_right_child); 
} 

static avl_node_t avl_balance_tree(avl_node_t *node) 
{ 
    int factor = avl_update_height_get_balance(*node); 

    if (factor > 1) { 
        if (avl_get_balance((*node)->avl_left_child) > 0) *node = avl_rotate_LL(*node); 
        else *node = avl_rotate_LR(*node); 
    } else if (factor < -1) { 
        if (avl_get_balance((*node)->avl_right_child) < 0) *node = avl_rotate_RR(*node); 
        else *node = avl_rotate_RL(*node); 
    }

    return *node; 
} 

#define AVL_ROOT(VARNAME) avl_node_t VARNAME = NULL;

#define AVL(NAME, STRUCT)                                                                   \
static inline int NAME##_AVL_compare(struct STRUCT *left, struct STRUCT *right);            \
avl_node_t NAME##_insert(avl_node_t *root, struct STRUCT *data) {                           \
    if (*root == NULL) {                                                                    \
        avl_node_t result = (avl_node_t)malloc(sizeof(struct avl_node)+                     \
                                               sizeof(struct STRUCT));                      \
        result->avl_left_child = result->avl_right_child = NULL;                            \
        result->avl_height = 1;                                                             \
        memcpy(result->data, data, sizeof(struct STRUCT));                                  \
        *root = result;                                                                     \
    } else {                                                                                \
        int cmp = NAME##_AVL_compare(data, (struct STRUCT*)&((*root)->data));               \
        if (cmp == 0) { /* Silently ignore */                                               \
            return *root;                                                                   \
        }                                                                                   \
        if (cmp < 0) {                                                                      \
            (*root)->avl_left_child = NAME##_insert(&(*root)->avl_left_child, data);        \
        } else {                                                                            \
            (*root)->avl_right_child = NAME##_insert(&(*root)->avl_right_child, data);      \
        }                                                                                   \
        (*root) = avl_balance_tree(root);                                                   \
    }                                                                                       \
    return *root;                                                                           \
}                                                                                           \
static int NAME##_search(avl_node_t node, struct STRUCT *data) {                            \
    while (node != NULL) {                                                                  \
        int result = NAME##_AVL_compare((struct STRUCT *)node->data, data);                 \
        if (result == 0) {                                                                  \
            memcpy(data, node->data, sizeof(struct STRUCT));                                \
            return 1;                                                                       \
        }                                                                                   \
        if (result < 0) node = node->avl_left_child;                                        \
        node = node->avl_right_child;                                                       \
    }                                                                                       \
    return 0;                                                                               \
}                                                                                           \
static void NAME##_free(avl_node_t *node) {                                                 \
    if (*node) {                                                                            \
        NAME##_free(&(*node)->avl_left_child);                                              \
        NAME##_free(&(*node)->avl_right_child);                                             \
        free(*node);                                                                        \
        *node = NULL;                                                                       \
    }                                                                                       \
}                                                                                           \
static inline int NAME##_AVL_compare(struct STRUCT *left, struct STRUCT *right)

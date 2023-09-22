#include <stdio.h>
#include "test_assert.h"
#include <sylvan_int.h>
#include <sylvan_align.h>

int test_forward_iterator(size_t i, size_t j, size_t size)
{
    bitmap_t bitmap = {
        .buckets = NULL,
        .size = 0
    };
    bitmap_init(&bitmap, size);

    for (size_t k = i; k < j; k++) {
        bitmap_set(&bitmap, k);
    }

    for (size_t k = i; k < j; k++) {
        assert(bitmap_get(&bitmap, k));
    }

    test_assert(bitmap_first(&bitmap) == i);

    size_t k = i;
    size_t index = bitmap_first(&bitmap);

    while (index != npos) {
        test_assert(index == k);
        index = bitmap_next(&bitmap, index);
        k++;
    }

    test_assert(bitmap_count(&bitmap) == j - i);

    bitmap_deinit(&bitmap);

    return 0;
}

int test_backwards_iterator(size_t i, size_t j, size_t size)
{
    bitmap_t bitmap = {
        .buckets = NULL,
        .size = 0
    };
    bitmap_init(&bitmap, size);

    for (size_t k = i; k < j; k++) {
        bitmap_set(&bitmap, k);
    }

    for (size_t k = i; k < j; k++) {
        assert(bitmap_get(&bitmap, k));
    }

    test_assert(bitmap_last(&bitmap) == j);

    size_t k = j;
    size_t index = bitmap_last(&bitmap);
    while (index != npos) {
        test_assert(index == k);
        index = bitmap_prev(&bitmap, index);
        k--;
    }

    test_assert(bitmap_count(&bitmap) == j - i);

    bitmap_deinit(&bitmap);

    return 0;
}

int test_atomic_forward_iterator(size_t i, size_t j, size_t size)
{
    atomic_bitmap_t bitmap = {
        .container = NULL,
        .size = 0
    };
    atomic_bitmap_init(&bitmap, size);

    for (size_t k = i; k < j; k++) {
        atomic_bitmap_set(&bitmap, k, memory_order_seq_cst);
    }

    for (size_t k = i; k < j; k++) {
        assert(atomic_bitmap_get(&bitmap, k, memory_order_seq_cst));
    }

    test_assert(atomic_bitmap_first(&bitmap) == i);

    size_t k = i;
    size_t index = atomic_bitmap_first(&bitmap);
    while (index != npos) {
        test_assert(index == k);
        index = atomic_bitmap_next(&bitmap, index);
        k++;
    }

    atomic_bitmap_deinit(&bitmap);

    return 0;
}

int test_atomic_backwards_iterator(size_t i, size_t j, size_t size)
{
    atomic_bitmap_t bitmap = {
        .container = NULL,
        .size = 0
    };
    atomic_bitmap_init(&bitmap, size);

    for (size_t k = i; k < j; k++) {
        atomic_bitmap_set(&bitmap, k, memory_order_relaxed);
    }

    for (size_t k = i; k < j; k++) {
        assert(atomic_bitmap_get(&bitmap, k, memory_order_seq_cst));
    }

    test_assert(atomic_bitmap_last(&bitmap) == j);

    size_t k = j;
    size_t index = atomic_bitmap_last(&bitmap);
    while (index != npos) {
        test_assert(index == k);
        index = atomic_bitmap_prev(&bitmap, index);
        k--;
    }

    atomic_bitmap_deinit(&bitmap);;

    return 0;
}

static inline size_t _rand()
{
    return rand() % 7919; // some not small prime number
}

int runtests(size_t ntests)
{
    printf("test_forward_iterator\n");
    for (size_t j = 0; j < ntests; j++) {
        size_t i = _rand();
        j = i + _rand();
        size_t size = j + 10;
        if (test_forward_iterator(i, j, size)) return 1;
    }
    printf("test_backwards_iterator\n");
    for (size_t j = 0; j < ntests; j++) {
        size_t i = _rand();
        j = i + _rand();
        size_t size = j + 10;
        if (test_backwards_iterator(i, j, size)) return 1;
    }
    printf("test_atomic_forward_iterator\n");
    for (size_t j = 0; j < ntests; j++) {
        size_t i = _rand();
        j = i + _rand();
        size_t size = j + 10;
        if (test_atomic_forward_iterator(i, j, size)) return 1;
    }
    printf("test_atomic_backwards_iterator\n");
    for (size_t j = 0; j < ntests; j++) {
        size_t i = _rand();
        j = i + _rand();
        size_t size = j + 10;
        if (test_atomic_backwards_iterator(i, j, size)) return 1;
    }
    return 0;
}

int main()
{
    return runtests(100);
}
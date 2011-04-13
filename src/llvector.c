#include "runtime.h"
#include "llvector.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

struct block {
	void *next; // next block
	uint8_t ref; // reference counter (gbc)
} __attribute__ ((aligned(8))); // align on 64-bits

llvector_t llvector_create(size_t datalength)
{
    llvector_t v = (llvector_t)rt_align(CACHE_LINE_SIZE, sizeof(struct llvector));

    llvector_init(v, datalength);

    return v;
}

void llvector_init(llvector_t v, size_t length)
{
    assert(length < 1000); // temporary!
    v->length = length;
    v->count = 0;
    v->lock = 0;
    v->data = 0;
    v->size = 0;

    int m = 4;
    v->blockSize = (sizeof(struct block) + length * m + CACHE_LINE_SIZE-1) / CACHE_LINE_SIZE;
    v->blockElements = (v->blockSize - sizeof(struct block)) / length;
}

void llvector_deinit(llvector_t v)
{
    // get lock
    while (1) {
        while (atomic8_read(&v->lock) != 0) {}
        if (cas(&v->lock, 0, 1)) break;
    }

    if (v->data != 0) {
        free(v->data);
        v->data = 0;
    }

    v->count = 0;
    v->size = 0;

    // release lock
    atomic8_write(&v->lock, 0);
}

void llvector_free(llvector_t v)
{
    llvector_deinit(v);
    free(v);
}

void llvector_get(llvector_t v, int item, void *data)
{
    // get lock
    while (1) {
        while (atomic8_read(&v->lock) != 0) {}
        if (cas(&v->lock, 0, 1)) break;
    }

    assert(item >= 0 && item < (signed)v->count);

    memcpy(data, v->data + item * v->length, v->length);

    // release lock
    atomic8_write(&v->lock, 0);
}

size_t llvector_count(llvector_t v)
{
    return v->count;
}

void llvector_add(llvector_t v, void *data)
{
    // get lock
    while (1) {
        while (atomic8_read(&v->lock) != 0) {}
        if (cas(&v->lock, 0, 1)) break;
    }

    if (v->data == NULL) {
        v->size = 32;
        v->data = rt_align(CACHE_LINE_SIZE, v->length * v->size);
    }

    memcpy(v->data + v->length * v->count, data, v->length);
    v->count++;
    if (v->count == v->size) {
        v->size <<= 1;
        v->data = realloc(v->data, v->size * v->length);
    }

    atomic8_write(&v->lock, 0);
}

void llvector_delete(llvector_t v, int item)
{
    // get lock
    while (1) {
        while (atomic8_read(&v->lock) != 0) {}
        if (cas(&v->lock, 0, 1)) break;
    }

    assert (item >= 0 && item < (signed)v->count);

    v->count--;
    if (item != (signed)v->count) {
        memcpy(v->data + item * v->length, v->data + v->count * v->length, v->length);
    }
/*
    if (v->count < (v->size>>2) && v->size>8) {
        v->size>>=1;
        v->data = realloc(v->data, v->size * v->length);
    }
*/
    atomic8_write(&v->lock, 0);
}

int llvector_pop(llvector_t v, void *data)
{
    // get lock
    while (1) {
        while (atomic8_read(&v->lock) != 0) {}
        if (cas(&v->lock, 0, 1)) break;
    }

    int good = 0;

    if (v->count > 0) {
        good = 1;

        v->count--;
        memcpy(data, v->data + v->count * v->length, v->length);

        if (v->count < (v->size>>2) && v->size>32) {
            v->size>>=1;
            v->data = realloc(v->data, v->size * v->length);
        }
    }

    atomic8_write(&v->lock, 0);

    return good;
}

void llvector_move(llvector_t from, llvector_t to)
{
    // NO LOCKING!
    assert(from->lock == 0);

    llvector_deinit(to);
    memcpy(to, from, sizeof(struct llvector));
    from->data = 0;
    from->size = 0;
    from->count = 0;
}

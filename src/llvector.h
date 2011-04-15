#include <stdint.h>

#ifndef LLVECTOR_H
#define LLVECTOR_H

struct llvector
{
    int32_t length;
    int32_t size;
    int32_t count;
    void* data;
    uint8_t lock;
};

typedef struct llvector* llvector_t;

llvector_t llvector_create(size_t datalength);
void llvector_init(llvector_t v, size_t length);
void llvector_deinit(llvector_t v);
void llvector_free(llvector_t v);

//void llvector_get(llvector_t v, int item, void *data);

/**
 * Copies an item from the vector to <data> and remove it.
 * Returns 1 if successful, 0 otherwise
 */
int llvector_pop(llvector_t v, void *data);

void llvector_push(llvector_t v, void *data);

int llvector_empty(llvector_t v);

size_t llvector_count(llvector_t v);


void llvector_delete(llvector_t v, int item);


/**
 * Moves all contents from one vector to another.
 */
void llvector_move(llvector_t from, llvector_t to);

#endif

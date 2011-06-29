#include <stdint.h>

#ifndef LLVECTOR2_H
#define LLVECTOR2_H

struct llvectorset
{
    int32_t datalength;
    int32_t blocklength;
    int32_t blockcount;

    void *data;
    int16_t count;
    int16_t next;

    struct llvectorset *nextset;
};

typedef struct llvectorset* llvectorset_t;

llvectorset_t llvectorset_create(int32_t datalength, int minimum);
void llvectorset_free(llvectorset_t v);

int llvectorset_pop(llvectorset_t v, void *block, void *data);

int llvectorset_empty(llvectorset_t v, void *block);

void llvectorset_push(llvectorset_t v, void *block, void *data);

#endif

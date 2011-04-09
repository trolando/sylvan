#include <stdint.h>

#ifndef LLSCHED
#define LLSCHED

struct llsched {
    int32_t threads;
    int32_t waitcount;
    size_t datasize; // 32 or 64
    uint8_t *flags; // 32 or 64
    void *data[]; // 32 or 64
};

typedef struct llsched* llsched_t;

llsched_t llsched_create(int threads, size_t datasize);
void llsched_free(llsched_t s);

void llsched_setupwait(llsched_t s);
void llsched_push(llsched_t s, int t, void* value);
int llsched_pop(llsched_t s, int t, void* value);

#endif

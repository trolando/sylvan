#include <stdint.h>

#ifndef LLSCHED
#define LLSCHED

struct llsched {
    int32_t threads;
    int32_t datasize;
    void *comm;
    void *data[]; // 32 or 64 (pointer)
};

typedef struct llsched* llsched_t;

/**
 * Create a new llsched queue for <threads> producer/consumers
 */
llsched_t llsched_create(int threads, size_t datasize);

/**
 * Free a llsched queue
 */
void llsched_free(llsched_t s);

/**
 * Wait until all other threads are in the WAITING state.
 * This method is called by the "master" thread just
 * before pushing the root job.
 */
void llsched_setupwait(llsched_t s);

/**
 * This method checks if a thread is waiting for a job. If a
 * thread is waiting for a job, this method will give it a job
 */
void llsched_check_waiting(llsched_t s, int t);

/**
 * Push a new job to the queue (tail)
 */
void llsched_push(llsched_t s, int t, void* value);

/**
 * Pop a job from the queue (tail)
 */
int llsched_pop(llsched_t s, int t, void* value);

#endif

#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "llsched.h"
#include "sylvan_runtime.h"

#ifndef SCHED_MINSIZE
#define SCHED_MINSIZE 256
#endif

#define SCHED_FLAG_RUNNING 0
#define SCHED_FLAG_WAITING 1
#define SCHED_FLAG_END 2

#define SCHED_DATA_EMPTY 0
#define SCHED_DATA_END 1
#define SCHED_DATA_FILLED 2

struct vector {
    int32_t head;
    int32_t tail;
    int32_t size;
    void *data;
};

typedef struct vector* vector_t;

struct comm {
    int32_t waitcount;
    uint8_t flags[]; // 32 or 64 (pointer)
};

typedef struct comm* comm_t;

llsched_t llsched_create(int threads, size_t datasize)
{
    llsched_t s = rt_align(CACHE_LINE_SIZE, sizeof(struct llsched) + sizeof(void*) * threads * 2);
    s->threads = threads;
    s->datasize = datasize;

    comm_t comm = rt_align(CACHE_LINE_SIZE, threads * sizeof(uint8_t) + sizeof(struct comm));
    s->comm = comm;

    comm->waitcount = 0;
    memset(&comm->flags[0], SCHED_FLAG_RUNNING, threads * sizeof(uint8_t));

    int i;
    for (i=0;i<threads;i++) {
        s->data[i] = (uint8_t*)rt_align(CACHE_LINE_SIZE, datasize + 1);
        *(uint8_t*)(s->data[i]) = SCHED_DATA_EMPTY;

        vector_t v = (vector_t)rt_align(CACHE_LINE_SIZE, sizeof(struct vector));
        v->head = 0;
        v->tail = 0;
        v->size = SCHED_MINSIZE;
        v->data = rt_align(CACHE_LINE_SIZE, v->size * datasize);
        s->data[threads+i] = v;
    }
    return s;
}

void llsched_free(llsched_t s)
{
    int i;
    for (i=0;i<s->threads;i++) {
        vector_t v = (vector_t)s->data[s->threads+i];
        if (v->data != 0) {
            free(v->data);
            v->data = 0;
        }
        free(v);
    }
    for (i=0;i<s->threads;i++) free(s->data[i]);
    free(s->comm);
    free(s);
}

void llsched_setupwait(llsched_t s)
{
    comm_t comm = (comm_t)s->comm;
    while (atomic8_read(&comm->waitcount) != (s->threads - 1)) {}
}

void llsched_check_waiting(llsched_t s, int t)
{
    vector_t v = (vector_t)s->data[s->threads+t];

    if (v->head == v->tail) return;

    comm_t comm = (comm_t)s->comm;

    if (atomic32_read(&comm->waitcount) == 0) return;

    int i;
    for (i=0;i<s->threads;i++) {
        if (atomic8_read(&comm->flags[i]) == SCHED_FLAG_WAITING) {
            if (cas(&comm->flags[i], SCHED_FLAG_WAITING, SCHED_FLAG_RUNNING)) {
                __sync_fetch_and_sub(&comm->waitcount, 1);
                memcpy(s->data[i]+1, v->data + s->datasize * v->head, s->datasize);
                atomic8_write(s->data[i], SCHED_DATA_FILLED);
                v->head++;
                return;
            }
        }
    }
}

static inline int llsched_wait(llsched_t s, const int t, void* value)
{
    // Current state is Running
    // First, set our own state...
    atomic8_write(s->data[t], SCHED_DATA_EMPTY);

    comm_t comm = (comm_t)s->comm;

    //__sync_bool_compare_and_swap(&comm->flags[t], SCHED_FLAG_RUNNING, SCHED_FLAG_WAITING);
    comm->flags[t] = SCHED_FLAG_WAITING;

    if (__sync_add_and_fetch(&comm->waitcount, 1) == s->threads) {
        // We know for certain that we are the only one detecting END.

        // Modify state (safe to do with just volatile instructions
        atomic32_write(&comm->waitcount, 0);
        int i;
        for (i=0; i<s->threads; i++)
        	if (t!=i)
        		atomic8_write(&comm->flags[i], SCHED_FLAG_END);

        // Release threads
        for (i=0; i<s->threads; i++)
        	if (t!=i)
        		atomic8_write(s->data[i], SCHED_DATA_END);

        atomic8_write(&comm->flags[t], SCHED_FLAG_RUNNING);
        return 0; // END!
    }

    // Wait for FILLED/END
    // Here, we are not wait-free
    while (atomic8_read(s->data[t]) == SCHED_DATA_EMPTY) {}

    if (atomic8_read(s->data[t]) == SCHED_DATA_END) {
        atomic8_write(&comm->flags[t], SCHED_FLAG_RUNNING);
        atomic8_write(s->data[t], SCHED_DATA_EMPTY);
        return 0; // END!
    }

    memcpy(value, s->data[t]+1, s->datasize);
    atomic8_write(s->data[t], SCHED_DATA_EMPTY);
    return 1; // DATA!
}

void llsched_push(llsched_t s, int t, void* value)
{
    assert(t>=0 && t < s->threads);
    vector_t v = (vector_t)s->data[s->threads+t];

    memcpy(v->data + v->tail * s->datasize, value, s->datasize);
    v->tail++;

    if (v->tail == (signed)v->size) {
    	v->size += (v->size >> 1); // increase by 50%
        v->data = realloc(v->data, v->size * s->datasize);
    }

    // there is at least one entry
    llsched_check_waiting(s, t);
}

int llsched_pop(llsched_t s, int t, void* value)
{
    assert(t>=0 && t < s->threads);
    vector_t v = (vector_t)s->data[s->threads+t];

    if (v->tail == v->head) {
        // request job from other threads
        return llsched_wait(s, t, value);
    }

    memcpy(value, v->data + (v->tail-1) * s->datasize, s->datasize);
    v->tail--;

    if (v->tail == v->head) {
    	v->head = v->tail = 0;
    } else {
    	llsched_check_waiting(s, t);
    }

    return 1;

    //size_t count = v->tail - v->head;
    /*
    if (v->size > 32 && count < (v->size>>2)) {
        if (v->head > 0) {
            memmove(v->data, v->data + v->head * s->datasize, count * s->datasize);
            v->tail -= v->head;
            v->head = 0;
        }
        v->size >>= 1;
        v->data = realloc(v->data, v->size * s->datasize);
    }
    */

    // only execute if count > 0
    //if (count > 0) llsched_check_waiting(s, t, v);

    //return 1;
}



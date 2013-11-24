
#ifndef BARRIER_H
#define BARRIER_H

#include <pthread.h>
#include <stdint.h>

#include <atomics.h>

#ifndef PTHREAD_BARRIER_SERIAL_THREAD
#   define PTHREAD_BARRIER_SERIAL_THREAD -1
#endif

#define MAX_THREADS 64

typedef union __attribute__((__packed__)) asize_u {
    size_t      val;
    // performance difference is minimal compared to reserving an entire line.
    char        pad[LINE_SIZE - sizeof(size_t)];
} asize_t;

typedef struct barrier_s barrier_t;
struct barrier_s {
    size_t       __attribute__ ((aligned(LINE_SIZE))) ids;
    size_t       __attribute__ ((aligned(LINE_SIZE))) threads;
    size_t       __attribute__ ((aligned(LINE_SIZE))) count;
    size_t       __attribute__ ((aligned(LINE_SIZE))) wait;
    /* the following is needed only for destroy: */
    asize_t             entered[MAX_THREADS];
    pthread_key_t       tls_key;
};

extern void barrier_init (barrier_t *b, unsigned count);

extern void barrier_destroy (barrier_t *b);

extern int barrier_wait (barrier_t *b);

#endif

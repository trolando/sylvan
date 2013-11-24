
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <barrier.h>

static inline size_t
get_next_id (barrier_t *b)
{
    size_t              val, new;
    do { // cas is faster than __sync_fetch_and_inc / __sync_inc_and_fetch
        val = ATOMIC_READ (b->ids);
        new = val + 1;
    } while ( !cas(&b->ids, val, new) );
    return val;
}

static inline int
get_id (barrier_t *b)
{
    int                *id = pthread_getspecific (b->tls_key);
    if (id == NULL) {
        id = malloc (sizeof(int));
        *id = get_next_id (b);
        pthread_setspecific (b->tls_key, id);
    }
    return *id;
}

int
barrier_wait (barrier_t *b)
{
    // get id ( only needed for destroy :( )
    int             id = get_id (b);

    // signal entry
    ATOMIC_WRITE (b->entered[id].val, 1);

    size_t          not = ATOMIC_READ (b->wait);
    if (b->threads == add_fetch(b->count, 1)) {
        ATOMIC_WRITE (b->count, 0); // reset counter
        ATOMIC_WRITE (b->wait, 1 - not); // flip wait
        ATOMIC_WRITE (b->entered[id].val, 0); // signal exit
        return PTHREAD_BARRIER_SERIAL_THREAD; // master return value
    } else {
        while (not == ATOMIC_READ(b->wait)) {} // wait
        ATOMIC_WRITE (b->entered[id].val, 0); // signal exit
        return 0; // slave return value
    }
}

void
barrier_init (barrier_t *b, unsigned count)
{
    assert (count <= MAX_THREADS);
    memset (b, 0, sizeof(barrier_t));
    pthread_key_create (&b->tls_key, free);
    b->threads = count;
}

void
barrier_destroy (barrier_t *b)
{
    // wait for all to exit
    size_t              i;
    for (i = 0; i < b->threads; i++)
        while (1 == b->entered[i].val) {}
    pthread_key_delete (b->tls_key);
}

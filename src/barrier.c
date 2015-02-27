/*
 * Copyright 2011-2014 Formal Methods and Tools, University of Twente
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sylvan_config.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <barrier.h>

static inline size_t
get_next_id(barrier_t *b)
{
    size_t val, new_val;
    do { // cas is faster than __sync_fetch_and_inc / __sync_inc_and_fetch
        val = ATOMIC_READ (b->ids);
        new_val = val + 1;
    } while (!cas(&b->ids, val, new_val));
    return val;
}

static inline int
get_id(barrier_t *b)
{
    int *id = (int*)pthread_getspecific(b->tls_key);
    if (id == NULL) {
        id = (int*)malloc(sizeof(int));
        *id = get_next_id(b);
        pthread_setspecific(b->tls_key, id);
    }
    return *id;
}

int
barrier_wait(barrier_t *b)
{
    // get id ( only needed for destroy :( )
    int id = get_id(b);

    // signal entry
    ATOMIC_WRITE (b->entered[id].val, 1);

    size_t wait = ATOMIC_READ(b->wait);
    if (b->threads == add_fetch(b->count, 1)) {
        ATOMIC_WRITE(b->count, 0); // reset counter
        ATOMIC_WRITE(b->wait, 1 - wait); // flip wait
        ATOMIC_WRITE(b->entered[id].val, 0); // signal exit
        return PTHREAD_BARRIER_SERIAL_THREAD; // master return value
    } else {
        while (wait == ATOMIC_READ(b->wait)) {} // wait
        ATOMIC_WRITE(b->entered[id].val, 0); // signal exit
        return 0; // slave return value
    }
}

void
barrier_init(barrier_t *b, unsigned count)
{
    assert(count <= MAX_THREADS);
    memset(b, 0, sizeof(barrier_t));
    pthread_key_create(&b->tls_key, free);
    b->threads = count;
}

void
barrier_destroy(barrier_t *b)
{
    // wait for all to exit
    size_t i;
    for (i=0; i<b->threads; i++)
        while (1 == b->entered[i].val) {}
    pthread_key_delete (b->tls_key);
}

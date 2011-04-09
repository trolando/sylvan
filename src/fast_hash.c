#include "fast_hash.h"

#include <stdlib.h>

#undef get16bits
#define get16bits(d) (*((const uint16_t *) (d)))

uint32_t
SuperFastHash (const void *data_, int len, uint32_t hash)
{
    const unsigned char *data = data_;
    uint32_t tmp;
    int rem;

    if (len <= 0 || data == NULL) return 0;

    rem = len & 3;
    len >>= 2;

    /* Main loop */
    for (;len > 0; len--) {
        hash  += get16bits (data);
        tmp    = (get16bits (data+2) << 11) ^ hash;
        hash   = (hash << 16) ^ tmp;
        data  += 2*sizeof (uint16_t);
        hash  += hash >> 11;
    }

    /* Handle end cases */
    switch (rem) {
        case 3: hash += get16bits (data);
                hash ^= hash << 16;
                hash ^= data[sizeof (uint16_t)] << 18;
                hash += hash >> 11;
                break;
        case 2: hash += get16bits (data);
                hash ^= hash << 11;
                hash += hash >> 17;
                break;
        case 1: hash += *data;
                hash ^= hash << 10;
                hash += hash >> 1;
    }

    /* Force "avalanching" of final 127 bits */
    hash ^= hash << 3;
    hash += hash >> 5;
    hash ^= hash << 4;
    hash += hash >> 17;
    hash ^= hash << 25;
    hash += hash >> 6;

    return hash;
}

/*
 * Bob Jenkins, <http://burtleburtle.net/bob/hash/doobs.html>
 * One-at-a-Time hash
 */
uint32_t
oat_hash (const void *data_, int len, uint32_t seed)
{
    const unsigned char *data = data_;
    unsigned             h = seed;
    int i;
    for (i = 0; i < len; i++) {
        h += data[i];
        h += (h << 10);
        h ^= (h >> 6);
    }
    h += (h << 3);
    h ^= (h >> 11);
    h += (h << 15);
    return h;
}

int
mix (int a, int b, int c)
{
    a = a - b; a = a - c; a = a ^ (((uint32_t) c) >> 13);
    b = b - c; b = b - a; b = b ^ (a << 8);
    c = c - a; c = c - b; c = c ^ (((uint32_t) b) >> 13);
    a = a - b; a = a - c; a = a ^ (((uint32_t) c) >> 12);
    b = b - c; b = b - a; b = b ^ (a << 16);
    c = c - a; c = c - b; c = c ^ (((uint32_t) b) >> 5);
    c = c - a; c = c - b; c = c ^ (((uint32_t) b) >> 15);
    return c;
}

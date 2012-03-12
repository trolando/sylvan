#include <stdint.h>

static void memxchg(void *one, void *two, size_t length)
{
    if ((length & ~7)) {
        // First, align one of the arrays - doesnt matter which one

        length -= ((size_t)one) & 7;

        if (((size_t)one) & 1) {
            register uint8_t t = *(uint8_t*)two;
            *((uint8_t*)two++) = *(uint8_t*)one;
            *((uint8_t*)one++) = t;
        }

        if (((size_t)one) & 2) {
            register uint16_t t = *(uint16_t*)two;
            *((uint16_t*)two) = *(uint16_t*)one;
            *((uint16_t*)one) = t;
            one += 2;
            two += 2;
        }
        
        if (((size_t)one) & 4) {
            register uint32_t t = *(uint32_t*)two;
            *((uint32_t*)two) = *(uint32_t*)one;
            *((uint32_t*)one) = t;
            one += 4;
            two += 4;
        }

        // After aligning, copy what's left in blocks of 8

        register size_t count = length >> 3;

        while (count) {
            register uint64_t t = *(uint64_t*)two;
            *((uint64_t*)two) = *(uint64_t*)one;
            *((uint64_t*)one) = t;
            one += 8;
            two += 8;
            count--;
        }
    }

    if (length & 4) {
        register uint32_t t = *(uint32_t*)two;
        *((uint32_t*)two) = *(uint32_t*)one;
        *((uint32_t*)one) = t;
        one += 4;
        two += 4;
    }
    
    if (length & 2) {
        register uint16_t t = *(uint16_t*)two;
        *((uint16_t*)two) = *(uint16_t*)one;
        *((uint16_t*)one) = t;
        one += 2;
        two += 2;
    }
        
    if (length & 1) {
        register uint8_t t = *(uint8_t*)two;
        *((uint8_t*)two++) = *(uint8_t*)one;
        *((uint8_t*)one++) = t;
    }
}

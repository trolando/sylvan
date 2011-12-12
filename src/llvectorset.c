#include "sylvan_runtime.h"
#include "llvector2.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define RESIZING ((void*)-1)
#define SETTING -1

// The size of the block without the data is 8 bytes. 
// If data size is e.g. 4 bytes, then with a cache line of 64 bytes, you can
// put 14 data entries in each block.
struct block {
  int16_t next_block; // next block index (chain)
  int16_t write_block; // last block index (for writing) - only valid for root
  int16_t read_block; // first block index (for reading) - only valid for root
  uint8_t head; // first valid item 
  uint8_t tail; // last valid item + 1
  uint8_t data[];
}; 

typedef struct block* block_t;

#define GET_BLOCK(v,a) ((block_t)((v)->data + (v)->blocklength * (a)))

/*
static inline increase_block_size(llvectorset_t v)
{
  void* cur_data = v->data;
  if (__sync_bool_compare_and_swap(&v->data, cur_data, RESIZING)) {
    cur_data = realloc(cur_data, v->count*v->blocklength);
    v->count<<=1;
    v->data = cur_data;
    return;
  }
  while (*((volatile void**)v->data) == RESIZING) {}
}

static inline block_t llblock_get_next(llvectorset_t v)
{
  if (v->next == v->count) {
    if (v->next == 0) {
      llvectorset_t v2 = (llvectorset_t)rt_align(CACHE_LINE_SIZE, sizeof(struct llvectorset));

      v2->datalength = v->datalength;
      v2->blocklength = v->blocklength;
      v2->blockcount = v->blockcount;

      v2->count = v->count*2;
      v2->next = 0; // next block
      v2->data = rt_align(CACHE_LINE_SIZE, v2->blocklength * v2->count);

      v2->nextset = 0;

      if (!__sync_bool_compare_and_swap(&v->nextset, 0, v2)) free(v2);
      v = v->nextset;
    }
  }
  int16_t next;
  while (true) {
    next = v->next;
    if (next >= v->count) increase_block_size(v);
    else if (__sync_bool_compare_and_swap(&v->next, next, next+1)) break;
  }

  void *cur_data;
  while (true)
    while (*((volatile void**)&v->data) == RESIZING) {}

}
*/

/**
 * Create a new vectorset. Each vector will have at least <minimum> entries.
 * The initial number of vector blocks is <initial>. The size of each entry in the
 * vectors is <datalength>.
 */
llvectorset_t llvectorset_create(int32_t datalength, int minimum, int initial)
{
  llvectorset_t v = (llvectorset_t)rt_align(CACHE_LINE_SIZE, sizeof(struct llvectorset));

  v->datalength = datalength;

  // Calculate size of each block with "minimum" entries in it
  int32_t tmp = sizeof(struct block) + datalength * minimum;
  // Very simple algorithm to determine number of entries per cache line. 
  // Just ceil() to cache line size
  // This will work well if your entry size is e.g. 4 bytes...
  tmp = (tmp + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE;
  v->blocklength = tmp;
  v->blockcount = (tmp - sizeof(struct block)) / length;
 
  if (v->blockcount > 254) v->blockcount = 254; // because it is uint8_t and 255 is highest value for tail.

  v->count = initial;
  v->next = 0; // next free block
  v->data = rt_align(CACHE_LINE_SIZE, v->blocklength * v->count);

  //v->nextset = 0;
}

void llvectorset_free(llvector_t v)
{
  free(v->data);
  free(v);
}

void llvectorset_reset(llvectorset_t v) 
{
  v->next = 0;
}

int16_t llvectorset_getnew(llvectorset_t v) 
{
  int16_t idx = __sync_fetch_and_add(&v->next, 1);
  if (idx == v->count) {
    // out of memory. todo: resize
    return -1;
  }

  block_t b = (block_t)(v->data + v->blocklength * idx);
  b->next_block = -1;
  b->read_block = idx; // me
  b->write_block = idx; // me
  b->head = 0;
  b->tail = 0;

  return idx;
}

int llvectorset_push(llvectorset_t v, int16_t idx, void *data)
{
  block_t block = GET_BLOCK(v, idx);
  
  // get write block
  int16_t wb = block->write_block;
  block_t write = GET_BLOCK(v, wb);

  while (true) {
    uint8_t idx = *(volatile uint8_t*)&write->tail;

    // check if out of space and someone else if already getting a new block
    if (idx == v->blockcount+1) {
      // wait until there is a new block...
      int16_t new_wb = *(volatile int16_t*)&block->write_block;
      while (new_wb == wb) new_wb = *(volatile int16_t*)&block->write_block;
      // other thread has updated!
      if (new_wb == -1) {
        return 0; // out of memory
      }
      wb = new_wb;
      write = GET_BLOCK(v, wb);
      continue; // try again
    }

    // try to own this idx
    if (!__sync_bool_compare_and_swap(&write->tail, idx, idx+1)) continue;

    // if out of space, get new block
    if (idx == v->blockcount) {
      // we own the last one, idx is now set to 255!
      int16_t next_block = llvectorset_getnew(v);
      if (v == -1) {
        block->write_block = -1;
        return 0; // out of memory
      }

      // order of memory assignments doesn't matter
      wb = next_block;
      block->write_block = wb;
      write->next_block = wb;
      write = GET_BLOCK(v, next_block);
      continue;      
    }

    // ok, success! write it
    memcpy(&write->data[0] + v->datalength * idx, data, v->datalength);
    return 1;
  }
  
}

int llvector_pop(llvector_t v, void *data)
{
  while (1) {
    block_t block = *(volatile block_t*)&v->readBlock;
    if (block == NULL) return 0; // no block => no data

    uint16_t start = block->start;
    if (start == v->blockElements) {
      // end of block!
      __sync_compare_and_swap(&v->readBlock, block, block->next);
      // try again
      continue;
    }

    if (start == block->pos) return 0; // start == pos => no data

    // wait until written
    while (start == atomic16_read(&block->end)) {}

    if (__sync_bool_compare_and_swap(&block->start, start, start+1)) {
      // we've claimed the entry!

      memcpy(data, &block->data[start * v->datalength], v->datalength);
      return 1;
    }

    // If unclaimed, try again
  }
}


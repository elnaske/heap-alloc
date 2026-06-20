#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t Header;

/* HeapChunk
 * A chunk of memory on the heap.
 *
 * header defines the size of the chunk.
 * Since chunk size has to be a multiple of 16,
 * the least significant bit of the header is used to
 * indicate whether the chunk is allocated (1) or free (0).
 * Each chunk also has a footer, which is an exact copy of
 * the header placed at the end of the chunk.
 *
 * next and prev are the next and previous element in the free list.
 * They are offsets from the start of the heap,
 * because representing them as 32-bit integers instead
 * of 64-bit pointers reduces the size of HeapChunk from 24 to 12.
 * NULL pointers are represented as UINT32_MAX.
 *
 * When a chunk is allocated, heap_alloc() returns a pointer
 * to the payload, i.e. just after the header.
 * Since only freed chunks need the prev and next pointers,
 * allocated chunks can overwrite them.
 * When freeing a chunk, the position of the previous chunks
 * can be inferred from that chunk's footer.
 *
 * This means that a whole chunk looks like this:
 * Free:        [ header | next | prev | footer ]
 * Allocated:   [ header | payload | footer ]
 *                       ^ ptr
 */
typedef struct HeapChunk {
    Header header;
    uint32_t next;
    uint32_t prev;
} HeapChunk;

typedef struct {
    HeapChunk *start;
    HeapChunk *first_free;
    uint32_t size;
    uint32_t avail;
    bool is_init;
} HeapData;

enum HEAP_STATUS {
    HEAP_OK,
    HEAP_ERR_MMAP,
    HEAP_ERR_NULL,
};

extern HeapData g_heap;

void *heap_alloc(uint32_t size);
void heap_free(void *ptr);
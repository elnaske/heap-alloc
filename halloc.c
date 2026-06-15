#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define ALIGN_TO_16(x) (((x) + 15) / 16 * 16)
#define PTR_TO_OFFSET(ptr, start) (uint32_t)((char *)(ptr) - (char *)(start))
#define OFFSET_TO_PTR(offset, start) (HeapChunk *)((char *)(start) + (offset))

#define Header uint32_t

static inline Header pack_header(uint32_t chunk_size, bool is_alloc) {
    return chunk_size | is_alloc;
}

static inline uint32_t unpack_chunk_size(Header header) {
    return (header >> 1) << 1;
}

enum HEAP_STATUS {
    HEAP_OK,
    HEAP_ERR_MMAP,
    HEAP_ERR_NULL,
};

/* HeapChunk
 * A chunk of memory on the heap.
 * 
 * header defines the size of the chunk.
 * Since chunk size has to be a multiple of 16,
 * the least significant bit of the header is used to
 * indicate whether the chunk is allocated (1) or free (0).
 *
 * next and prev are offsets from the start of the heap,
 * because representing them as 32-bit integers instead
 * of 64-bit pointers reduces the size of HeapChunk from 24 to 12.
 *
 * NULL pointers are represented as 0 and are contextually
 * distinguished from an actual 0 offset.
 * next can't point backwards, so 0 will always be interpreted as NULL.
 * prev can only be NULL if it's the first chunk.
 * 
 * When a chunk is allocated, heap_alloc() returns a pointer
 * to the payload, i.e. just after the chunk.
 * (Since there is no footer, allocated chunks also need to keep
 * the offsets for when they're freed)
*/
typedef struct HeapChunk {
    Header header;
    uint32_t next;
    uint32_t prev;
} HeapChunk;

typedef struct {
    HeapChunk *start;
    HeapChunk *first_free;
    uint32_t avail;
} HeapData;

HeapData g_heap;
bool g_heap_is_init;

int heap_init(HeapData *heap) {
    if (!heap) return HEAP_ERR_NULL;

    int heap_size = getpagesize();
    void *heap_start = mmap(NULL, heap_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (heap_start == (void *)-1) {
        return HEAP_ERR_MMAP;
    }

    heap->start = heap_start;
    heap->first_free = heap_start;
    heap->avail = (uint32_t)heap_size;

    HeapChunk chunk = {
        .header = pack_header(heap->avail, false),
        .prev = 0,
        .next = 0,
    };

    *(heap->first_free) = chunk;

    g_heap_is_init = true;

    return HEAP_OK;
}

HeapChunk *find_free_chunk(uint32_t alloc_size) {
    HeapChunk *curr = g_heap.first_free;

    while (curr) {
        if (unpack_chunk_size(curr->header) >= alloc_size) {
            return curr;
        }
        curr = OFFSET_TO_PTR(curr->next, g_heap.start);
    }

    return NULL;
}

void *heap_alloc(uint32_t size) {
    if (!g_heap_is_init) {
        if (heap_init(&g_heap) != HEAP_OK) {
            return NULL;
        }
    }

    uint32_t aligned_size = ALIGN_TO_16(size + sizeof(HeapChunk));

    if (g_heap.avail >= aligned_size) {
        HeapChunk *alloced_chunk = find_free_chunk(aligned_size);

        if (alloced_chunk) {
            uint32_t remainder_size = unpack_chunk_size(alloced_chunk->header) - aligned_size;

            alloced_chunk->header = pack_header(aligned_size, true);

            if (remainder_size >= sizeof(HeapChunk)) {
                HeapChunk free_remainder = {
                    .header = pack_header(remainder_size, false),
                    .next = 0,
                    .prev = PTR_TO_OFFSET(alloced_chunk, g_heap.start),
                };

                alloced_chunk->next = PTR_TO_OFFSET((char *)alloced_chunk + aligned_size, g_heap.start);
                *OFFSET_TO_PTR(alloced_chunk->next, g_heap.start) = free_remainder;

                g_heap.first_free = OFFSET_TO_PTR(alloced_chunk->next, g_heap.start);
                g_heap.avail -= aligned_size;
            } else {
                g_heap.first_free = NULL;
                g_heap.avail = 0;
            }

            // skip chunk, return address to payload
            return (char *)alloced_chunk + sizeof(HeapChunk);
        }
    }

    return NULL;
}

void heap_free(void *ptr);

int main() {
    heap_init(&g_heap);

    printf("Heap start: %p\n", (void *)g_heap.start);
    printf("First free chunk: %p\n", (void *)g_heap.first_free);
    printf("Available memory: %d\n", g_heap.avail);
    printf("First chunk->size: %d\n", unpack_chunk_size(g_heap.first_free->header));

    HeapChunk *ptr = heap_alloc(4000);
    printf("\n");
    printf("Allocated addr: %p\n", (void *)ptr);
    if (ptr)
        printf("Allocated size: %d\n", unpack_chunk_size(*(Header *)((char *)ptr - sizeof(HeapChunk))));

    printf("\n");
    printf("First free chunk: %p\n", (void *)g_heap.first_free);
    printf("Available memory: %d\n", g_heap.avail);
    if (g_heap.first_free) {
        printf("First chunk->size: %d\n", unpack_chunk_size(g_heap.first_free->header));
        printf("First chunk->prev: %p\n", (void *)OFFSET_TO_PTR(g_heap.first_free->prev, g_heap.start));
    }

    ptr = heap_alloc(1);
    printf("\n");
    printf("Allocated addr: %p\n", (void *)ptr);
    if (ptr)
        printf("Allocated size: %d\n", unpack_chunk_size(*(Header *)((char *)ptr - sizeof(HeapChunk))));

    printf("\n");
    printf("First free chunk: %p\n", (void *)g_heap.first_free);
    printf("Available memory: %d\n", g_heap.avail);
    if (g_heap.first_free) {
        printf("First chunk->size: %d\n", unpack_chunk_size(g_heap.first_free->header));
        printf("First chunk->prev: %p\n", (void *)OFFSET_TO_PTR(g_heap.first_free->prev, g_heap.start));
    }

    return HEAP_OK;
}
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define ALIGN_TO_16(x) (((x) + 15) / 16 * 16)
#define NULL_OFFSET UINT32_MAX

typedef uint32_t Header;

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

static inline uint32_t ptr_to_offset(void *ptr) {
    if (!ptr) return NULL_OFFSET;
    return (char *)ptr - (char *)g_heap.start;
}

static inline void *offset_to_ptr(uint32_t offset) {
    if (offset == NULL_OFFSET) return NULL;
    return (char *)g_heap.start + offset;
}

static inline void place_footer(HeapChunk *chunk_start, Header header) {
    uint32_t size = unpack_chunk_size(header);

    *(Header *)((char *)chunk_start + size - sizeof(Header)) = header;
}

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
        .prev = NULL_OFFSET,
        .next = NULL_OFFSET,
    };

    *(heap->first_free) = chunk;
    place_footer(heap->start, chunk.header);

    g_heap_is_init = true;

    return HEAP_OK;
}

HeapChunk *find_free_chunk(uint32_t alloc_size) {
    HeapChunk *curr = g_heap.first_free;

    while (curr) {
        if (unpack_chunk_size(curr->header) >= alloc_size) {
            return curr;
        }
        curr = offset_to_ptr(curr->next);
    }

    return NULL;
}

void *heap_alloc(uint32_t size) {
    if (!g_heap_is_init) {
        if (heap_init(&g_heap) != HEAP_OK) {
            return NULL;
        }
    }

    uint32_t aligned_size = ALIGN_TO_16(size + sizeof(Header));

    if (g_heap.avail >= aligned_size) {
        HeapChunk *alloced_chunk = find_free_chunk(aligned_size);

        if (alloced_chunk) {
            // remove allocated chunk from free list
            HeapChunk *prev = offset_to_ptr(alloced_chunk->prev);
            HeapChunk *next = offset_to_ptr(alloced_chunk->next);
            if (prev)
                prev->next = alloced_chunk->next;
            else
                g_heap.first_free = next;
            if (next) next->prev = alloced_chunk->prev;

            // truncate allocated chunk
            uint32_t remainder_size = unpack_chunk_size(alloced_chunk->header) - aligned_size;
            alloced_chunk->header = pack_header(aligned_size, true);
            place_footer(alloced_chunk, alloced_chunk->header);

            if (remainder_size >= sizeof(HeapChunk)) {
                HeapChunk free_remainder = {
                    .header = pack_header(remainder_size, false),
                    .next = ptr_to_offset(g_heap.first_free),
                    .prev = NULL_OFFSET,
                };

                // place new free block
                HeapChunk *next_free = (HeapChunk *)((char *)alloced_chunk + aligned_size);
                *next_free = free_remainder;
                place_footer(next_free, free_remainder.header);

                if (g_heap.first_free)
                    g_heap.first_free->prev = ptr_to_offset(next_free);
                g_heap.first_free = next_free;
                g_heap.avail -= aligned_size;
            } else {
                g_heap.first_free = NULL;
                g_heap.avail = 0;
            }

            // skip header, return address to payload
            return (char *)alloced_chunk + sizeof(Header);
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

    HeapChunk *ptr = heap_alloc(1);
    printf("\n");
    printf("Allocated addr: %p\n", (void *)ptr);
    if (ptr)
        printf("Allocated size: %d\n", unpack_chunk_size(*(Header *)((char *)ptr - sizeof(Header))));

    printf("\n");
    printf("First free chunk: %p\n", (void *)g_heap.first_free);
    printf("Available memory: %d\n", g_heap.avail);
    if (g_heap.first_free) {
        printf("First chunk size: %d\n", unpack_chunk_size(g_heap.first_free->header));
        printf("First chunk->next: %p\n", offset_to_ptr(g_heap.first_free->next));
        if (g_heap.first_free > g_heap.start) {
            printf("Previous chunk size: %d\n", unpack_chunk_size(*((Header *)(g_heap.first_free) - 1)));
        }
    }

    ptr = heap_alloc(74);
    printf("\n");
    printf("Allocated addr: %p\n", (void *)ptr);
    if (ptr)
        printf("Allocated size: %d\n", unpack_chunk_size(*(Header *)((char *)ptr - sizeof(Header))));

    printf("\n");
    printf("First free chunk: %p\n", (void *)g_heap.first_free);
    printf("Available memory: %d\n", g_heap.avail);
    if (g_heap.first_free) {
        printf("First chunk size: %d\n", unpack_chunk_size(g_heap.first_free->header));
        printf("First chunk->next: %p\n", offset_to_ptr(g_heap.first_free->next));
        if (g_heap.first_free > g_heap.start) {
            printf("Previous chunk size: %d\n", unpack_chunk_size(*((Header *)(g_heap.first_free) - 1)));
        }
    }

    return HEAP_OK;
}
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
    return header & ~1;
}

static inline bool is_alloc(Header header) {
    return header & 1;
}

#define MARK_ALLOC(x) (x) |= 1
#define MARK_FREE(x) (x) &= ~1

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
    bool is_init;
} HeapData;

enum HEAP_STATUS {
    HEAP_OK,
    HEAP_ERR_MMAP,
    HEAP_ERR_NULL,
};

HeapData g_heap;

static inline uint32_t ptr_to_offset(void *ptr) {
    if (!ptr) return NULL_OFFSET;

    return (char *)ptr - (char *)g_heap.start;
}

static inline void *offset_to_ptr(uint32_t offset) {
    if (offset == NULL_OFFSET) return NULL;

    return (char *)g_heap.start + offset;
}

static inline int place_footer(HeapChunk *chunk_start, Header header) {
    if (!chunk_start) return HEAP_ERR_NULL;

    uint32_t size = unpack_chunk_size(header);
    *(Header *)((char *)chunk_start + size - sizeof(Header)) = header;

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

int free_list_remove(HeapChunk *chunk) {
    if (!chunk) return HEAP_ERR_NULL;

    HeapChunk *prev = offset_to_ptr(chunk->prev);
    HeapChunk *next = offset_to_ptr(chunk->next);

    if (prev)
        prev->next = chunk->next;
    else
        g_heap.first_free = next;
    if (next) next->prev = chunk->prev;

    MARK_ALLOC(chunk->header);

    return HEAP_OK;
}

int free_list_prepend(HeapChunk *chunk) {
    if (!chunk) return HEAP_ERR_NULL;

    if (g_heap.first_free)
        g_heap.first_free->prev = ptr_to_offset(chunk);

    g_heap.first_free = chunk;

    MARK_FREE(chunk->header);

    return HEAP_OK;
}

int truncate_chunk(HeapChunk *chunk, uint32_t size) {
    if (!chunk) return HEAP_ERR_NULL;

    chunk->header = pack_header(size, is_alloc(chunk->header));
    place_footer(chunk, chunk->header);

    return HEAP_OK;
}

int add_free_chunk(HeapChunk *addr, uint32_t size, HeapChunk *next) {
    if (!addr) return HEAP_ERR_NULL;

    HeapChunk chunk = {
        .header = pack_header(size, false),
        .next = ptr_to_offset(next),
        .prev = NULL_OFFSET,
    };

    *addr = chunk;
    place_footer(addr, chunk.header);

    free_list_prepend(addr);

    return HEAP_OK;
}

int heap_init(HeapData *heap) {
    if (!heap) return HEAP_ERR_NULL;

    uint32_t heap_size = getpagesize();
    void *heap_start = mmap(NULL, heap_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (heap_start == (void *)-1)
        return HEAP_ERR_MMAP;

    heap->start = heap_start;
    heap->first_free = NULL; // will be set by free_list_prepend() in add_free_chunk()
    heap->avail = heap_size;

    add_free_chunk(heap_start, heap_size, NULL);

    g_heap.is_init = true;

    return HEAP_OK;
}

void *heap_alloc(uint32_t size) {
    if (!g_heap.is_init) {
        if (heap_init(&g_heap) != HEAP_OK)
            return NULL;
    }

    uint32_t aligned_size = ALIGN_TO_16(size + sizeof(Header));

    if (g_heap.avail >= aligned_size) {
        HeapChunk *alloced_chunk = find_free_chunk(aligned_size);

        if (alloced_chunk) {
            free_list_remove(alloced_chunk);

            uint32_t curr_size = unpack_chunk_size(alloced_chunk->header);

            if (aligned_size < curr_size) {
                truncate_chunk(alloced_chunk, aligned_size);
                uint32_t remainder_size = curr_size - aligned_size;

                HeapChunk *next_free = (HeapChunk *)((char *)alloced_chunk + aligned_size);
                add_free_chunk(next_free, remainder_size, g_heap.first_free);
            }

            g_heap.avail -= aligned_size;

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

    HeapChunk *ptr = heap_alloc(4000);
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
            printf("Previous chunk is_alloc: %d\n", is_alloc(*((Header *)(g_heap.first_free) - 1)));
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
            printf("Previous chunk is_alloc: %d\n", is_alloc(*((Header *)(g_heap.first_free) - 1)));
        }
    }

    return HEAP_OK;
}
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define ALIGN_TO_16(x) (((x) + 15) / 16 * 16)
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

typedef struct HeapChunk {
    Header header;
    struct HeapChunk *next;
    struct HeapChunk *prev;
} HeapChunk;

typedef struct {
    HeapChunk *start;
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
    heap->avail = (uint32_t)heap_size - sizeof(Header);

    HeapChunk chunk = {
        .header = pack_header(heap->avail, false),
        .prev = NULL,
        .next = NULL,
    };

    *(heap->start) = chunk;

    g_heap_is_init = true;

    return HEAP_OK;
}

HeapChunk *find_free_chunk(uint32_t alloc_size) {
    HeapChunk *curr = g_heap.start;

    while (curr) {
        if (unpack_chunk_size(curr->header) >= alloc_size) {
            return curr;
        }
        curr = curr->next;
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
            uint32_t remainder_size = unpack_chunk_size(alloced_chunk->header) - aligned_size;
            HeapChunk free_remainder = {
                .header = pack_header(remainder_size, false),
                .next = NULL,
                .prev = alloced_chunk,
            };

            alloced_chunk->header = pack_header(aligned_size, true);
            alloced_chunk->next = (HeapChunk *)((char *)alloced_chunk + aligned_size);

            *(alloced_chunk->next) = free_remainder;

            g_heap.start = alloced_chunk->next;
            g_heap.avail -= aligned_size;

            return (char *)alloced_chunk + sizeof(Header); // advance pointer to payload
        }
    }

    return NULL;
}

void heap_free(void *ptr);

int main() {
    heap_init(&g_heap);

    printf("Heap start: %p\n", (void *)g_heap.start);
    printf("Available memory: %d\n", g_heap.avail);
    printf("First chunk->size: %d\n", unpack_chunk_size(g_heap.start->header));

    HeapChunk *ptr = heap_alloc(32);
    printf("\n");
    printf("Allocated addr: %p\n", (void *)ptr);
    printf("Allocated size: %d\n", unpack_chunk_size(*(Header *)((char *)ptr - sizeof(Header))));

    printf("\n");
    printf("Heap start: %p\n", (void *)g_heap.start);
    printf("Available memory: %d\n", g_heap.avail);
    printf("First chunk->size: %d\n", unpack_chunk_size(g_heap.start->header));
    printf("First chunk->prev: %p\n", (void *)g_heap.start->prev);

    ptr = heap_alloc(1);
    printf("\n");
    printf("Allocated addr: %p\n", (void *)ptr);
    printf("Allocated size: %d\n", unpack_chunk_size(*(Header *)((char *)ptr - sizeof(Header))));

    printf("\n");
    printf("Heap start: %p\n", (void *)g_heap.start);
    printf("Available memory: %d\n", g_heap.avail);
    printf("First chunk->size: %d\n", unpack_chunk_size(g_heap.start->header));
    printf("First chunk->prev: %p\n", (void *)g_heap.start->prev);

    return HEAP_OK;
}
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <unistd.h>

enum HEAP_STATUS {
    HEAP_OK,
    HEAP_ERR_MMAP,
};

typedef struct HeapChunk {
    uint32_t size;
    bool free;
    struct HeapChunk *next;
    struct HeapChunk *prev;
} HeapChunk;

typedef struct {
    HeapChunk *start;
    uint32_t avail;
} HeapData;


int heap_init(HeapData *heap) {
    int heap_size = getpagesize();
    void *heap_start = mmap(NULL, heap_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (heap_start == (void *)-1) {
        return HEAP_ERR_MMAP;
    }

    heap->start = heap_start;
    heap->avail = (uint32_t)heap_size - sizeof(HeapChunk);
    
    HeapChunk chunk = {
        .size = heap->avail,
        .free = true,
        .prev = NULL,
        .next = NULL,
    };

    *(heap->start) = chunk;

    return HEAP_OK;
}

void *heap_alloc(uint32_t size);

void heap_free(void *ptr);

int main() {
    HeapData heap;
    heap_init(&heap);

    printf("Heap start: %p\n", (void *)heap.start);
    printf("Available memory: %d\n", heap.avail);
    printf("First chunk->size: %d\n", heap.start->size);

    return HEAP_OK;
}
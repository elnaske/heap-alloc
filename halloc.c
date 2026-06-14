#include<stdio.h>
#include<stdint.h>
#include<sys/mman.h>
#include<unistd.h>

enum HEAP_STATUS {
    HEAP_OK,
    HEAP_ERR_MMAP,
};

void *heap_alloc(size_t size);

void heap_free(void *ptr);

int main() {
    int heap_size = getpagesize();
    void *heap_start = mmap(NULL, heap_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (!heap_start) {
        return HEAP_ERR_MMAP;
    }

    printf("Heap start: %p\n", heap_start);
    printf("Heap size: %d\n", heap_size);

    return HEAP_OK;
}
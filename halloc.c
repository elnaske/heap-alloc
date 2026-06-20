#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define ALIGNMENT 16
#define ALIGN(x) (((x) + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT)
#define NULL_OFFSET UINT32_MAX

typedef uint32_t Header;

static inline Header pack_header(uint32_t chunk_size, bool is_alloc) {
    return (chunk_size & ~1) | is_alloc;
}

static inline uint32_t unpack_size(Header header) {
    return header & ~1;
}

static inline bool is_alloc(Header header) {
    return header & 1;
}

static inline void add_header_size(Header *header, uint32_t add_size) {
    if (!header) return;

    *header = pack_header(unpack_size(*header) + add_size, is_alloc(*header));
}

#define HEADER_SIZE sizeof(Header)
#define FOOTER_SIZE sizeof(Header)

#define MARK_ALLOC(x) (x) |= 1
#define MARK_FREE(x) (x) &= ~1

#define ALLOC 1
#define FREE 0

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

#define MIN_CHUNK_SIZE ALIGN(sizeof(HeapChunk) + FOOTER_SIZE)

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

HeapData g_heap;

static inline uint32_t ptr_to_offset(void *ptr) {
    if (!ptr) return NULL_OFFSET;

    return (char *)ptr - (char *)g_heap.start;
}

static inline void *offset_to_ptr(uint32_t offset) {
    if (offset == NULL_OFFSET) return NULL;

    return (char *)g_heap.start + offset;
}

int place_footer(HeapChunk *chunk_start, Header header) {
    if (!chunk_start) return HEAP_ERR_NULL;

    uint32_t size = unpack_size(header);
    *(Header *)((char *)chunk_start + size - FOOTER_SIZE) = header;

    return HEAP_OK;
}

int change_alloc_status(HeapChunk *chunk, bool alloc_status) {
    if (!chunk) return HEAP_ERR_NULL;

    Header *footer = (Header *)((char *)chunk + unpack_size(chunk->header) - FOOTER_SIZE);

    if (alloc_status == ALLOC) {
        MARK_ALLOC(chunk->header);
        MARK_ALLOC(*footer);
    } else {
        MARK_FREE(chunk->header);
        MARK_FREE(*footer);
    }

    return HEAP_OK;
}

HeapChunk *find_free_chunk(uint32_t alloc_size) {
    HeapChunk *curr = g_heap.first_free;

    while (curr) {
        if (unpack_size(curr->header) >= alloc_size) {
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

    if (next)
        next->prev = chunk->prev;

    g_heap.avail -= unpack_size(chunk->header);

    change_alloc_status(chunk, ALLOC);

    return HEAP_OK;
}

int free_list_prepend(HeapChunk *chunk) {
    if (!chunk) return HEAP_ERR_NULL;

    change_alloc_status(chunk, FREE);

    chunk->prev = NULL_OFFSET;
    chunk->next = NULL_OFFSET;

    if (g_heap.first_free) {
        chunk->next = ptr_to_offset(g_heap.first_free);
        g_heap.first_free->prev = ptr_to_offset(chunk);
    }

    g_heap.first_free = chunk;
    g_heap.avail += unpack_size(chunk->header);

    return HEAP_OK;
}

int truncate_chunk(HeapChunk *chunk, uint32_t size) {
    if (!chunk) return HEAP_ERR_NULL;

    chunk->header = pack_header(size, is_alloc(chunk->header));
    place_footer(chunk, chunk->header);

    return HEAP_OK;
}

int add_free_chunk(HeapChunk *addr, uint32_t size) {
    if (!addr) return HEAP_ERR_NULL;

    HeapChunk chunk = {
        .header = pack_header(size, FREE),
        .next = NULL_OFFSET,
        .prev = NULL_OFFSET,
    };

    *addr = chunk;
    place_footer(addr, chunk.header);

    free_list_prepend(addr);

    return HEAP_OK;
}

int heap_init() {
    uint32_t heap_size = getpagesize();
    void *heap_start = mmap(NULL, heap_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (heap_start == (void *)-1)
        return HEAP_ERR_MMAP;

    g_heap.start = heap_start;
    g_heap.size = heap_size;
    g_heap.avail = 0;

    add_free_chunk(heap_start, heap_size); // also sets g_heap.first_free and g_heap.avail

    g_heap.is_init = true;

    return HEAP_OK;
}

void *heap_alloc(uint32_t size) {
    if (!g_heap.is_init) {
        if (heap_init(&g_heap) != HEAP_OK)
            return NULL;
    }

    uint32_t aligned_size = ALIGN(size + HEADER_SIZE + FOOTER_SIZE);

    if (aligned_size >= MIN_CHUNK_SIZE && g_heap.avail >= aligned_size) {
        HeapChunk *alloced_chunk = find_free_chunk(aligned_size);

        if (alloced_chunk) {
            free_list_remove(alloced_chunk);

            uint32_t curr_size = unpack_size(alloced_chunk->header);

            if (aligned_size < curr_size) {
                truncate_chunk(alloced_chunk, aligned_size);
                uint32_t remainder_size = curr_size - aligned_size;

                HeapChunk *next_free = (HeapChunk *)((char *)alloced_chunk + aligned_size);
                add_free_chunk(next_free, remainder_size);
            }

            // skip header, return address to payload
            return (char *)alloced_chunk + HEADER_SIZE;
        }
    }

    return NULL;
}

void heap_free(void *ptr) {
    if (!ptr || ptr < (void *)g_heap.start || ptr >= (void *)(g_heap.start + g_heap.size)) return;

    HeapChunk *chunk = (HeapChunk *)((char *)ptr - HEADER_SIZE);

    if (is_alloc(chunk->header)) {
        uint32_t freed_size = unpack_size(chunk->header);

        Header *prev = &chunk->header - 1; // previous chunk's footer
        Header *next = (Header *)(void *)((char *)chunk + unpack_size(chunk->header));

        bool prev_free = (char *)prev >= (char *)g_heap.start && !is_alloc(*prev);
        bool next_free = ptr_to_offset(next) < g_heap.size && !is_alloc(*next);

        HeapChunk *prev_chunk = NULL;
        HeapChunk *next_chunk = NULL;

        if (prev_free)
            prev_chunk = (HeapChunk *)((char *)chunk - unpack_size(*prev));
        if (next_free)
            next_chunk = (HeapChunk *)next;

        if (prev_free && next_free) {
            uint32_t next_size = unpack_size(next_chunk->header);

            free_list_remove(next_chunk);

            add_header_size(&prev_chunk->header, freed_size + next_size);
            place_footer(prev_chunk, prev_chunk->header);

            g_heap.avail += freed_size + next_size;

        } else if (prev_free) {
            add_header_size(&prev_chunk->header, freed_size);
            place_footer(prev_chunk, prev_chunk->header);

            g_heap.avail += freed_size;

        } else if (next_free) {
            free_list_remove(next_chunk);

            add_header_size(&chunk->header, unpack_size(next_chunk->header));
            free_list_prepend(chunk);

        } else {
            free_list_prepend(chunk);
        }
    }

    return;
}

static void print_free_list(HeapData *heap) {
    if (!heap) return;

    HeapChunk *curr = heap->first_free;

    if (!curr)
        printf("(empty)\n");

    while (curr) {
        printf("Addr: %p\tSize: %u\n", (void *)curr, unpack_size(curr->header));
        curr = offset_to_ptr(curr->next);
    }
    printf("\n");
}

int main() {
    heap_init(&g_heap);

    printf("Heap start: %p\n", (void *)g_heap.start);
    printf("Available memory: %d\n", g_heap.avail);

    print_free_list(&g_heap);

    void *ptr = heap_alloc(8);
    printf("Allocated addr: %p\n", ptr);
    if (ptr)
        printf("Allocated size: %d\n", unpack_size(*(Header *)((char *)ptr - HEADER_SIZE)));

    printf("Available memory: %d\n", g_heap.avail);
    print_free_list(&g_heap);

    void *ptr2 = heap_alloc(30);
    printf("\n");
    printf("Allocated addr: %p\n", ptr2);
    if (ptr2)
        printf("Allocated size: %d\n", unpack_size(*(Header *)((char *)ptr2 - HEADER_SIZE)));

    printf("Available memory: %d\n", g_heap.avail);
    print_free_list(&g_heap);

    void *ptr3 = heap_alloc(8);
    printf("\n");
    printf("Allocated addr: %p\n", ptr3);
    if (ptr3)
        printf("Allocated size: %d\n", unpack_size(*(Header *)((char *)ptr3 - HEADER_SIZE)));

    printf("Available memory: %d\n", g_heap.avail);
    print_free_list(&g_heap);

    void *ptr4 = heap_alloc(20);
    printf("\n");
    printf("Allocated addr: %p\n", ptr4);
    if (ptr4)
        printf("Allocated size: %d\n", unpack_size(*(Header *)((char *)ptr4 - HEADER_SIZE)));

    printf("Available memory: %d\n", g_heap.avail);
    print_free_list(&g_heap);

    void *ptr5 = heap_alloc(3976);
    printf("\n");
    printf("Allocated addr: %p\n", ptr5);
    if (ptr5)
        printf("Allocated size: %d\n", unpack_size(*(Header *)((char *)ptr5 - HEADER_SIZE)));

    printf("Available memory: %d\n", g_heap.avail);
    print_free_list(&g_heap);

    printf("\n");
    printf("FREEING: %p\n", (void *)ptr);
    heap_free(ptr);

    printf("Available memory: %d\n", g_heap.avail);
    print_free_list(&g_heap);

    printf("\n");
    printf("FREEING: %p\n", (void *)ptr5);
    heap_free(ptr5);

    printf("Available memory: %d\n", g_heap.avail);
    print_free_list(&g_heap);

    printf("\n");
    printf("FREEING: %p\n", (void *)ptr2);
    heap_free(ptr2);

    printf("Available memory: %d\n", g_heap.avail);
    print_free_list(&g_heap);

    printf("\n");
    printf("FREEING: %p\n", (void *)ptr4);
    heap_free(ptr4);

    printf("Available memory: %d\n", g_heap.avail);
    print_free_list(&g_heap);

    printf("\n");
    printf("FREEING: %p\n", (void *)ptr3);
    heap_free(ptr3);

    printf("Available memory: %d\n", g_heap.avail);
    print_free_list(&g_heap);

    return HEAP_OK;
}
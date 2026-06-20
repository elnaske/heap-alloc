#include <stdbool.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include "halloc.h"

#define ALIGNMENT 16
#define ALIGN(x) (((x) + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT)
#define NULL_OFFSET UINT32_MAX

#define HEADER_SIZE sizeof(Header)
#define FOOTER_SIZE sizeof(Header)
#define MIN_CHUNK_SIZE ALIGN(sizeof(HeapChunk) + FOOTER_SIZE)

#define MARK_ALLOC(x) (x) |= 1
#define MARK_FREE(x) (x) &= ~1

#define ALLOC 1
#define FREE 0

HeapData g_heap;

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

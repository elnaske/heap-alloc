#include <stdio.h>
#include <string.h>

#include "halloc.h"

int main() {
    // Example usage
    char *str = "Hello";
    unsigned len = strlen(str) + 1;

    char *ptr = heap_alloc(len);

    memcpy(ptr, str, len);

    printf("%s from the heap!\n", ptr);

    heap_free(ptr);
}
# heap-alloc

A simple heap allocator written in C.
The design is largely based on the one presented in ch. 9.9 of [CS:APP](https://csapp.cs.cmu.edu/) (boundary tags, first fit, immediate coalescing) but with an explicit free list.

## Usage

`halloc.h` exposes two functions:

```c
void *heap_alloc(uint32_t size);
void heap_free(void *ptr);
```

They can be used in the same way as `malloc()` and `free()`.
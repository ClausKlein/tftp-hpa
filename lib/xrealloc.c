/*
 * xrealloc.c
 *
 * Simple error-checking version of realloc()
 *
 */

#include "../config.h"

void *xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size);

    if (!p) {
        fprintf(stderr, "Out of memory!\n");
        exit(128);
    }

    return p;
}

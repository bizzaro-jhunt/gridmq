/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "alloc.h"

#if defined GRID_ALLOC_MONITOR

#include "mutex.h"
#include "int.h"

#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>

struct grid_alloc_hdr {
    size_t size;
    const char *name;
};

static struct grid_mutex grid_alloc_sync;
static size_t grid_alloc_bytes;
static size_t grid_alloc_blocks;

void grid_alloc_init (void)
{
    grid_mutex_init (&grid_alloc_sync);
    grid_alloc_bytes = 0;
    grid_alloc_blocks = 0;
}

void grid_alloc_term (void)
{
    grid_mutex_term (&grid_alloc_sync);
}

void *grid_alloc_ (size_t size, const char *name)
{
    uint8_t *chunk;

    chunk = malloc (sizeof (struct grid_alloc_hdr) + size);
    if (!chunk)
        return NULL;

    grid_mutex_lock (&grid_alloc_sync);
    ((struct grid_alloc_hdr*) chunk)->size = size;
    ((struct grid_alloc_hdr*) chunk)->name = name;
    grid_alloc_bytes += size;
    ++grid_alloc_blocks;
    printf ("Allocating %s (%zu bytes)\n", name, size);
    printf ("Current memory usage: %zu bytes in %zu blocks\n",
        grid_alloc_bytes, grid_alloc_blocks);
    grid_mutex_unlock (&grid_alloc_sync);

    return chunk + sizeof (struct grid_alloc_hdr);
}

void *grid_realloc (void *ptr, size_t size)
{
    struct grid_alloc_hdr *oldchunk;
    struct grid_alloc_hdr *newchunk;
    size_t oldsize;

    oldchunk = ((struct grid_alloc_hdr*) ptr) - 1;
    oldsize = oldchunk->size;
    newchunk = realloc (oldchunk, sizeof (struct grid_alloc_hdr) + size);
    if (!newchunk)
        return NULL;
    newchunk->size = size;

    grid_mutex_lock (&grid_alloc_sync);
    grid_alloc_bytes -= oldsize;
    grid_alloc_bytes += size;
    printf ("Reallocating %s (%zu bytes to %zu bytes)\n",
        newchunk->name, oldsize, size);
    printf ("Current memory usage: %zu bytes in %zu blocks\n",
        grid_alloc_bytes, grid_alloc_blocks);
    grid_mutex_unlock (&grid_alloc_sync);

    return newchunk + sizeof (struct grid_alloc_hdr);
}

void grid_free (void *ptr)
{
    struct grid_alloc_hdr *chunk;
    
    if (!ptr)
        return;
    chunk = ((struct grid_alloc_hdr*) ptr) - 1;

    grid_mutex_lock (&grid_alloc_sync);
    grid_alloc_bytes -= chunk->size;
    --grid_alloc_blocks;
    printf ("Deallocating %s (%zu bytes)\n", chunk->name, chunk->size);
    printf ("Current memory usage: %zu bytes in %zu blocks\n",
        grid_alloc_bytes, grid_alloc_blocks);
    grid_mutex_unlock (&grid_alloc_sync);

    free (chunk);
}

#else

#include <stdlib.h>

void grid_alloc_init (void)
{
}

void grid_alloc_term (void)
{
}

void *grid_alloc_ (size_t size)
{
    return malloc (size);
}

void *grid_realloc (void *ptr, size_t size)
{
    return realloc (ptr, size);
}

void grid_free (void *ptr)
{
    free (ptr);
}

#endif


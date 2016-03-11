/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.
    Copyright (c) 2014 Achille Roussel All rights reserved.

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

#include "chunk.h"
#include "atomic.h"
#include "alloc.h"
#include "fast.h"
#include "wire.h"
#include "err.h"

#include <string.h>

#define GRID_CHUNK_TAG 0xdeadcafe
#define GRID_CHUNK_TAG_DEALLOCATED 0xbeadfeed

typedef void (*grid_chunk_free_fn) (void *p);

struct grid_chunk {

    /*  Number of places the chunk is referenced from. */
    struct grid_atomic refcount;

    /*  Size of the message in bytes. */
    size_t size;

    /*  Deallocation function. */
    grid_chunk_free_fn ffn;

    /*  The structure if followed by optional empty space, a 32 bit unsigned
        integer specifying the size of said empty space, a 32 bit tag and
        the message data itself. */
};

/*  Private functions. */
static struct grid_chunk *grid_chunk_getptr (void *p);
static void *grid_chunk_getdata (struct grid_chunk *c);
static void grid_chunk_default_free (void *p);
static size_t grid_chunk_hdrsize ();

int grid_chunk_alloc (size_t size, int type, void **result)
{
    size_t sz;
    struct grid_chunk *self;
    const size_t hdrsz = grid_chunk_hdrsize ();

    /*  Compute total size to be allocated. Check for overflow. */
    sz = hdrsz + size;
    if (grid_slow (sz < hdrsz))
        return -ENOMEM;

    /*  Allocate the actual memory depending on the type. */
    switch (type) {
    case 0:
        self = grid_alloc (sz, "message chunk");
        break;
    default:
        return -EINVAL;
    }
    if (grid_slow (!self))
        return -ENOMEM;

    /*  Fill in the chunk header. */
    grid_atomic_init (&self->refcount, 1);
    self->size = size;
    self->ffn = grid_chunk_default_free;

    /*  Fill in the size of the empty space between the chunk header
        and the message. */
    grid_putl ((uint8_t*) ((uint32_t*) (self + 1)), 0);

    /*  Fill in the tag. */
    grid_putl ((uint8_t*) ((((uint32_t*) (self + 1))) + 1), GRID_CHUNK_TAG);

    *result = grid_chunk_getdata (self);
    return 0;
}

int grid_chunk_realloc (size_t size, void **chunk)
{
    struct grid_chunk *self;
    struct grid_chunk *new_chunk;
    void *new_ptr;
    size_t hdr_size;
    size_t new_size;
    int rc;

    self = grid_chunk_getptr (*chunk);

    /*  Check if we only have one reference to this object, in that case we can
        reallocate the memory chunk. */
    if (self->refcount.n == 1) {

        /* Compute new size, check for overflow. */
        hdr_size = grid_chunk_hdrsize ();
        new_size = hdr_size + size;
        if (grid_slow (new_size < hdr_size))
            return -ENOMEM;

        /*  Reallocate memory chunk. */
        new_chunk = grid_realloc (self, new_size);
        if (grid_slow (new_chunk == NULL))
            return -ENOMEM;

        new_chunk->size = size;
        *chunk = grid_chunk_getdata (new_chunk);
    }

    /*  There are many references to this memory chunk, we have to create a new
        one and copy the data. */
    else {
        new_ptr = NULL;
        rc = grid_chunk_alloc (size, 0, &new_ptr);

        if (grid_slow (rc != 0)) {
            return rc;
        }

        memcpy (new_ptr, grid_chunk_getdata (self), self->size);
        *chunk = new_ptr;
        grid_atomic_dec (&self->refcount, 1);
    }

    return 0;
}

void grid_chunk_free (void *p)
{
    struct grid_chunk *self;

    self = grid_chunk_getptr (p);

    /*  Decrement the reference count. Actual deallocation happens only if
        it drops to zero. */
    if (grid_atomic_dec (&self->refcount, 1) <= 1) {

        /*  Mark chunk as deallocated. */
        grid_putl ((uint8_t*) (((uint32_t*) p) - 1), GRID_CHUNK_TAG_DEALLOCATED);

        /*  Deallocate the resources held by the chunk. */
        grid_atomic_term (&self->refcount);

        /*  Deallocate the memory block according to the allocation
            mechanism specified. */
        self->ffn (self);
    }
}

void grid_chunk_addref (void *p, uint32_t n)
{
    struct grid_chunk *self;

    self = grid_chunk_getptr (p);

    grid_atomic_inc (&self->refcount, n);
}


size_t grid_chunk_size (void *p)
{
    return grid_chunk_getptr (p)->size;
}

void *grid_chunk_trim (void *p, size_t n)
{
    struct grid_chunk *self;
    const size_t hdrsz = sizeof (struct grid_chunk) + 2 * sizeof (uint32_t);
    size_t empty_space;

    self = grid_chunk_getptr (p);

    /*  Sanity check. We cannot trim more bytes than there are in the chunk. */
    grid_assert (n <= self->size);

    /*  Adjust the chunk header. */
    p = ((uint8_t*) p) + n;
    grid_putl ((uint8_t*) (((uint32_t*) p) - 1), GRID_CHUNK_TAG);
    empty_space = (uint8_t*) p - (uint8_t*) self - hdrsz;
    grid_assert(empty_space < UINT32_MAX);
    grid_putl ((uint8_t*) (((uint32_t*) p) - 2), (uint32_t) empty_space);

    /*  Adjust the size of the message. */
    self->size -= n;

    return p;
}

static struct grid_chunk *grid_chunk_getptr (void *p)
{
    uint32_t off;

    grid_assert (grid_getl ((uint8_t*) p - sizeof (uint32_t)) == GRID_CHUNK_TAG);
    off = grid_getl ((uint8_t*) p - 2 * sizeof (uint32_t));

    return (struct  grid_chunk*) ((uint8_t*) p - 2 *sizeof (uint32_t) - off -
        sizeof (struct grid_chunk));
}

static void *grid_chunk_getdata (struct grid_chunk *self)
{
    return ((uint8_t*) (self + 1)) + 2 * sizeof (uint32_t);
}

static void grid_chunk_default_free (void *p)
{
    grid_free (p);
}

static size_t grid_chunk_hdrsize ()
{
    return sizeof (struct grid_chunk) + 2 * sizeof (uint32_t);
}


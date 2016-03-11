/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.

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

#include "chunkref.h"
#include "err.h"

#include <string.h>

/*  grid_chunkref should be reinterpreted as this structure in case the first
    byte ('tag') is 0xff. */
struct grid_chunkref_chunk {
    uint8_t tag;
    void *chunk;
};

/*  Check whether VSM are small enough for size to fit into the first byte
    of the structure. */
CT_ASSERT (GRID_CHUNKREF_MAX < 255);

/*  Check whether grid_chunkref_chunk fits into grid_chunkref. */
CT_ASSERT (sizeof (struct grid_chunkref) >= sizeof (struct grid_chunkref_chunk));

void grid_chunkref_init (struct grid_chunkref *self, size_t size)
{
    int rc;
    struct grid_chunkref_chunk *ch;

    if (size < GRID_CHUNKREF_MAX) {
        self->u.ref [0] = (uint8_t) size;
        return;
    }

    ch = (struct grid_chunkref_chunk*) self;
    ch->tag = 0xff;
    rc = grid_chunk_alloc (size, 0, &ch->chunk);
    errno_assert (rc == 0);
}

void grid_chunkref_init_chunk (struct grid_chunkref *self, void *chunk)
{
    struct grid_chunkref_chunk *ch;

    ch = (struct grid_chunkref_chunk*) self;
    ch->tag = 0xff;
    ch->chunk = chunk;
}

void grid_chunkref_term (struct grid_chunkref *self)
{
    struct grid_chunkref_chunk *ch;

    if (self->u.ref [0] == 0xff) {
        ch = (struct grid_chunkref_chunk*) self;
        grid_chunk_free (ch->chunk);
    }
}

void *grid_chunkref_getchunk (struct grid_chunkref *self)
{
    int rc;
    struct grid_chunkref_chunk *ch;
    void *chunk;

    if (self->u.ref [0] == 0xff) {
        ch = (struct grid_chunkref_chunk*) self;
        self->u.ref [0] = 0;
        return ch->chunk;
    }

    rc = grid_chunk_alloc (self->u.ref [0], 0, &chunk);
    errno_assert (rc == 0);
    memcpy (chunk, &self->u.ref [1], self->u.ref [0]);
    self->u.ref [0] = 0;
    return chunk;
}

void grid_chunkref_mv (struct grid_chunkref *dst, struct grid_chunkref *src)
{
    memcpy (dst, src, src->u.ref [0] == 0xff ?
        (int)sizeof (struct grid_chunkref_chunk) : src->u.ref [0] + 1);
}

void grid_chunkref_cp (struct grid_chunkref *dst, struct grid_chunkref *src)
{
    struct grid_chunkref_chunk *ch;

    if (src->u.ref [0] == 0xff) {
        ch = (struct grid_chunkref_chunk*) src;
        grid_chunk_addref (ch->chunk, 1);
    }
    memcpy (dst, src, sizeof (struct grid_chunkref));
}

void *grid_chunkref_data (struct grid_chunkref *self)
{
    return self->u.ref [0] == 0xff ?
        ((struct grid_chunkref_chunk*) self)->chunk :
        &self->u.ref [1];
}

size_t grid_chunkref_size (struct grid_chunkref *self)
{
    return self->u.ref [0] == 0xff ?
        grid_chunk_size (((struct grid_chunkref_chunk*) self)->chunk) :
        self->u.ref [0];
}

void grid_chunkref_trim (struct grid_chunkref *self, size_t n)
{
    struct grid_chunkref_chunk *ch;

    if (self->u.ref [0] == 0xff) {
        ch = (struct grid_chunkref_chunk*) self;
        ch->chunk = grid_chunk_trim (ch->chunk, n);
        return;
    }

    grid_assert (self->u.ref [0] >= n);
    memmove (&self->u.ref [1], &self->u.ref [1 + n], self->u.ref [0] - n);
    self->u.ref [0] -= (uint8_t) n;
}

void grid_chunkref_bulkcopy_start (struct grid_chunkref *self, uint32_t copies)
{
    struct grid_chunkref_chunk *ch;

    if (self->u.ref [0] == 0xff) {
        ch = (struct grid_chunkref_chunk*) self;
        grid_chunk_addref (ch->chunk, copies);
    }
}

void grid_chunkref_bulkcopy_cp (struct grid_chunkref *dst, struct grid_chunkref *src)
{
    memcpy (dst, src, sizeof (struct grid_chunkref));
}


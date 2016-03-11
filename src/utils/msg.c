/*
    Copyright (c) 2012-2013 Martin Sustrik  All rights reserved.

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

#include "msg.h"

#include <string.h>

void grid_msg_init (struct grid_msg *self, size_t size)
{
    grid_chunkref_init (&self->sphdr, 0);
    grid_chunkref_init (&self->hdrs, 0);
    grid_chunkref_init (&self->body, size);
}

void grid_msg_init_chunk (struct grid_msg *self, void *chunk)
{
    grid_chunkref_init (&self->sphdr, 0);
    grid_chunkref_init (&self->hdrs, 0);
    grid_chunkref_init_chunk (&self->body, chunk);
}

void grid_msg_term (struct grid_msg *self)
{
    grid_chunkref_term (&self->sphdr);
    grid_chunkref_term (&self->hdrs);
    grid_chunkref_term (&self->body);
}

void grid_msg_mv (struct grid_msg *dst, struct grid_msg *src)
{
    grid_chunkref_mv (&dst->sphdr, &src->sphdr);
    grid_chunkref_mv (&dst->hdrs, &src->hdrs);
    grid_chunkref_mv (&dst->body, &src->body);
}

void grid_msg_cp (struct grid_msg *dst, struct grid_msg *src)
{
    grid_chunkref_cp (&dst->sphdr, &src->sphdr);
    grid_chunkref_cp (&dst->hdrs, &src->hdrs);
    grid_chunkref_cp (&dst->body, &src->body);
}

void grid_msg_bulkcopy_start (struct grid_msg *self, uint32_t copies)
{
    grid_chunkref_bulkcopy_start (&self->sphdr, copies);
    grid_chunkref_bulkcopy_start (&self->hdrs, copies);
    grid_chunkref_bulkcopy_start (&self->body, copies);
}

void grid_msg_bulkcopy_cp (struct grid_msg *dst, struct grid_msg *src)
{
    grid_chunkref_bulkcopy_cp (&dst->sphdr, &src->sphdr);
    grid_chunkref_bulkcopy_cp (&dst->hdrs, &src->hdrs);
    grid_chunkref_bulkcopy_cp (&dst->body, &src->body);
}

void grid_msg_replace_body (struct grid_msg *self, struct grid_chunkref new_body) 
{
    grid_chunkref_term (&self->body);
    self->body = new_body;
}


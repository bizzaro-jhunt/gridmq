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

#include "msgqueue.h"

#include "../../utils/alloc.h"
#include "../../utils/fast.h"
#include "../../utils/err.h"

#include <string.h>

void grid_msgqueue_init (struct grid_msgqueue *self, size_t maxmem)
{
    struct grid_msgqueue_chunk *chunk;

    self->count = 0;
    self->mem = 0;
    self->maxmem = maxmem;

    chunk = grid_alloc (sizeof (struct grid_msgqueue_chunk), "msgqueue chunk");
    alloc_assert (chunk);
    chunk->next = NULL;

    self->out.chunk = chunk;
    self->out.pos = 0;
    self->in.chunk = chunk;
    self->in.pos = 0;

    self->cache = NULL;
}

void grid_msgqueue_term (struct grid_msgqueue *self)
{
    int rc;
    struct grid_msg msg;

    /*  Deallocate messages in the pipe. */
    while (1) {
        rc = grid_msgqueue_recv (self, &msg);
        if (rc == -EAGAIN)
            break;
        errnum_assert (rc >= 0, -rc);
        grid_msg_term (&msg);
    }

    /*  There are no more messages in the pipe so there's at most one chunk
        in the queue. Deallocate it. */
    grid_assert (self->in.chunk == self->out.chunk);
    grid_free (self->in.chunk);

    /*  Deallocate the cached chunk, if any. */
    if (self->cache)
        grid_free (self->cache);
}

int grid_msgqueue_empty (struct grid_msgqueue *self)
{
    return self->count == 0 ? 1 : 0;
}

int grid_msgqueue_send (struct grid_msgqueue *self, struct grid_msg *msg)
{
    size_t msgsz;

    /*  By allowing one message of arbitrary size to be written to the queue,
        we allow even messages that exceed max buffer size to pass through.
        Beyond that we'll apply the buffer limit as specified by the user. */
    msgsz = grid_chunkref_size (&msg->sphdr) + grid_chunkref_size (&msg->body);
    if (grid_slow (self->count > 0 && self->mem + msgsz >= self->maxmem))
        return -EAGAIN;

    /*  Adjust the statistics. */
    ++self->count;
    self->mem += msgsz;

    /*  Move the content of the message to the pipe. */
    grid_msg_mv (&self->out.chunk->msgs [self->out.pos], msg);
    ++self->out.pos;

    /*  If there's no space for a new message in the pipe, either re-use
        the cache chunk or allocate a new chunk if it does not exist. */
    if (grid_slow (self->out.pos == GRID_MSGQUEUE_GRANULARITY)) {
        if (grid_slow (!self->cache)) {
            self->cache = grid_alloc (sizeof (struct grid_msgqueue_chunk),
                "msgqueue chunk");
            alloc_assert (self->cache);
            self->cache->next = NULL;
        }
        self->out.chunk->next = self->cache;
        self->out.chunk = self->cache;
        self->cache = NULL;
        self->out.pos = 0;
    }

    return 0;
}

int grid_msgqueue_recv (struct grid_msgqueue *self, struct grid_msg *msg)
{
    struct grid_msgqueue_chunk *o;

    /*  If there is no message in the queue. */
    if (grid_slow (!self->count))
        return -EAGAIN;

    /*  Move the message from the pipe to the user. */
    grid_msg_mv (msg, &self->in.chunk->msgs [self->in.pos]);

    /*  Move to the next position. */
    ++self->in.pos;
    if (grid_slow (self->in.pos == GRID_MSGQUEUE_GRANULARITY)) {
        o = self->in.chunk;
        self->in.chunk = self->in.chunk->next;
        self->in.pos = 0;
        if (grid_fast (!self->cache))
            self->cache = o;
        else
            grid_free (o);
    }

    /*  Adjust the statistics. */
    --self->count;
    self->mem -= (grid_chunkref_size (&msg->sphdr) +
        grid_chunkref_size (&msg->body));

    return 0;
}


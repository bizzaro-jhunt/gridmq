/*
    Copyright (c) 2012-2014 Martin Sustrik  All rights reserved.

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

#include "xrep.h"

#include "../../grid.h"
#include "../../reqrep.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/alloc.h"
#include "../../utils/random.h"
#include "../../utils/wire.h"
#include "../../utils/list.h"
#include "../../utils/attr.h"

#include <string.h>

/*  Private functions. */
static void grid_xrep_destroy (struct grid_sockbase *self);

static const struct grid_sockbase_vfptr grid_xrep_sockbase_vfptr = {
    NULL,
    grid_xrep_destroy,
    grid_xrep_add,
    grid_xrep_rm,
    grid_xrep_in,
    grid_xrep_out,
    grid_xrep_events,
    grid_xrep_send,
    grid_xrep_recv,
    grid_xrep_setopt,
    grid_xrep_getopt
};

void grid_xrep_init (struct grid_xrep *self, const struct grid_sockbase_vfptr *vfptr,
    void *hint)
{
    grid_sockbase_init (&self->sockbase, vfptr, hint);

    /*  Start assigning keys beginning with a random number. This way there
        are no key clashes even if the executable is re-started. */
    grid_random_generate (&self->next_key, sizeof (self->next_key));

    grid_hash_init (&self->outpipes);
    grid_fq_init (&self->inpipes);
}

void grid_xrep_term (struct grid_xrep *self)
{
    grid_fq_term (&self->inpipes);
    grid_hash_term (&self->outpipes);
    grid_sockbase_term (&self->sockbase);
}

static void grid_xrep_destroy (struct grid_sockbase *self)
{
    struct grid_xrep *xrep;

    xrep = grid_cont (self, struct grid_xrep, sockbase);

    grid_xrep_term (xrep);
    grid_free (xrep);
}

int grid_xrep_add (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xrep *xrep;
    struct grid_xrep_data *data;
    int rcvprio;
    size_t sz;

    xrep = grid_cont (self, struct grid_xrep, sockbase);

    sz = sizeof (rcvprio);
    grid_pipe_getopt (pipe, GRID_SOL_SOCKET, GRID_RCVPRIO, &rcvprio, &sz);
    grid_assert (sz == sizeof (rcvprio));
    grid_assert (rcvprio >= 1 && rcvprio <= 16);

    data = grid_alloc (sizeof (struct grid_xrep_data), "pipe data (xrep)");
    alloc_assert (data);
    data->pipe = pipe;
    grid_hash_item_init (&data->outitem);
    data->flags = 0;
    grid_hash_insert (&xrep->outpipes, xrep->next_key & 0x7fffffff,
        &data->outitem);
    ++xrep->next_key;
    grid_fq_add (&xrep->inpipes, &data->initem, pipe, rcvprio);
    grid_pipe_setdata (pipe, data);

    return 0;
}

void grid_xrep_rm (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xrep *xrep;
    struct grid_xrep_data *data;

    xrep = grid_cont (self, struct grid_xrep, sockbase);
    data = grid_pipe_getdata (pipe);

    grid_fq_rm (&xrep->inpipes, &data->initem);
    grid_hash_erase (&xrep->outpipes, &data->outitem);
    grid_hash_item_term (&data->outitem);

    grid_free (data);
}

void grid_xrep_in (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xrep *xrep;
    struct grid_xrep_data *data;

    xrep = grid_cont (self, struct grid_xrep, sockbase);
    data = grid_pipe_getdata (pipe);

    grid_fq_in (&xrep->inpipes, &data->initem);
}

void grid_xrep_out (GRID_UNUSED struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xrep_data *data;

    data = grid_pipe_getdata (pipe);
    data->flags |= GRID_XREP_OUT;
}

int grid_xrep_events (struct grid_sockbase *self)
{
    return (grid_fq_can_recv (&grid_cont (self, struct grid_xrep,
        sockbase)->inpipes) ? GRID_SOCKBASE_EVENT_IN : 0) | GRID_SOCKBASE_EVENT_OUT;
}

int grid_xrep_send (struct grid_sockbase *self, struct grid_msg *msg)
{
    int rc;
    uint32_t key;
    struct grid_xrep *xrep;
    struct grid_xrep_data *data;

    xrep = grid_cont (self, struct grid_xrep, sockbase);

    /*  We treat invalid peer ID as if the peer was non-existent. */
    if (grid_slow (grid_chunkref_size (&msg->sphdr) < sizeof (uint32_t))) {
        grid_msg_term (msg);
        return 0;
    }

    /*  Retrieve the destination peer ID. Trim it from the header. */
    key = grid_getl (grid_chunkref_data (&msg->sphdr));
    grid_chunkref_trim (&msg->sphdr, 4);

    /*  Find the appropriate pipe to send the message to. If there's none,
        or if it's not ready for sending, silently drop the message. */
    data = grid_cont (grid_hash_get (&xrep->outpipes, key), struct grid_xrep_data,
        outitem);
    if (!data || !(data->flags & GRID_XREP_OUT)) {
        grid_msg_term (msg);
        return 0;
    }

    /*  Send the message. */
    rc = grid_pipe_send (data->pipe, msg);
    errnum_assert (rc >= 0, -rc);
    if (rc & GRID_PIPE_RELEASE)
        data->flags &= ~GRID_XREP_OUT;

    return 0;
}

int grid_xrep_recv (struct grid_sockbase *self, struct grid_msg *msg)
{
    int rc;
    struct grid_xrep *xrep;
    struct grid_pipe *pipe;
    int i;
    void *data;
    size_t sz;
    struct grid_chunkref ref;
    struct grid_xrep_data *pipedata;

    xrep = grid_cont (self, struct grid_xrep, sockbase);

    rc = grid_fq_recv (&xrep->inpipes, msg, &pipe);
    if (grid_slow (rc < 0))
        return rc;

    if (!(rc & GRID_PIPE_PARSED)) {

        /*  Determine the size of the message header. */
        data = grid_chunkref_data (&msg->body);
        sz = grid_chunkref_size (&msg->body);
        i = 0;
        while (1) {

            /*  Ignore the malformed requests without the bottom of the stack. */
            if (grid_slow ((i + 1) * sizeof (uint32_t) > sz)) {
                grid_msg_term (msg);
                return -EAGAIN;
            }

            /*  If the bottom of the backtrace stack is reached, proceed. */
            if (grid_getl ((uint8_t*)(((uint32_t*) data) + i)) & 0x80000000)
                break;

            ++i;
        }
        ++i;

        /*  Split the header and the body. */
        grid_assert (grid_chunkref_size (&msg->sphdr) == 0);
        grid_chunkref_term (&msg->sphdr);
        grid_chunkref_init (&msg->sphdr, i * sizeof (uint32_t));
        memcpy (grid_chunkref_data (&msg->sphdr), data, i * sizeof (uint32_t));
        grid_chunkref_trim (&msg->body, i * sizeof (uint32_t));
    }

    /*  Prepend the header by the pipe key. */
    pipedata = grid_pipe_getdata (pipe);
    grid_chunkref_init (&ref,
        grid_chunkref_size (&msg->sphdr) + sizeof (uint32_t));
    grid_putl (grid_chunkref_data (&ref), pipedata->outitem.key);
    memcpy (((uint8_t*) grid_chunkref_data (&ref)) + sizeof (uint32_t),
        grid_chunkref_data (&msg->sphdr), grid_chunkref_size (&msg->sphdr));
    grid_chunkref_term (&msg->sphdr);
    grid_chunkref_mv (&msg->sphdr, &ref);

    return 0;
}

int grid_xrep_setopt (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED int level, GRID_UNUSED int option,
    GRID_UNUSED const void *optval, GRID_UNUSED size_t optvallen)
{
    return -ENOPROTOOPT;
}

int grid_xrep_getopt (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED int level, GRID_UNUSED int option,
    GRID_UNUSED void *optval, GRID_UNUSED size_t *optvallen)
{
    return -ENOPROTOOPT;
}

static int grid_xrep_create (void *hint, struct grid_sockbase **sockbase)
{
    struct grid_xrep *self;

    self = grid_alloc (sizeof (struct grid_xrep), "socket (xrep)");
    alloc_assert (self);
    grid_xrep_init (self, &grid_xrep_sockbase_vfptr, hint);
    *sockbase = &self->sockbase;

    return 0;
}

int grid_xrep_ispeer (int socktype)
{
    return socktype == GRID_REQ ? 1 : 0;
}

static struct grid_socktype grid_xrep_socktype_struct = {
    AF_SP_RAW,
    GRID_REP,
    0,
    grid_xrep_create,
    grid_xrep_ispeer,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_socktype *grid_xrep_socktype = &grid_xrep_socktype_struct;


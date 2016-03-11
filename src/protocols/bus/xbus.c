/*
    Copyright (c) 2013-2014 Martin Sustrik  All rights reserved.

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

#include "xbus.h"

#include "../../grid.h"
#include "../../bus.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"
#include "../../utils/int.h"
#include "../../utils/attr.h"

#include <stddef.h>
#include <string.h>

/*  To make the algorithm super efficient we directly cast pipe pointers to
    pipe IDs (rather than maintaining a hash table). For this to work, it is
    neccessary for the pointer to fit in 64-bit ID. */
CT_ASSERT (sizeof (uint64_t) >= sizeof (struct grid_pipe*));

/*  Implementation of grid_sockbase's virtual functions. */
static void grid_xbus_destroy (struct grid_sockbase *self);
static const struct grid_sockbase_vfptr grid_xbus_sockbase_vfptr = {
    NULL,
    grid_xbus_destroy,
    grid_xbus_add,
    grid_xbus_rm,
    grid_xbus_in,
    grid_xbus_out,
    grid_xbus_events,
    grid_xbus_send,
    grid_xbus_recv,
    grid_xbus_setopt,
    grid_xbus_getopt
};

void grid_xbus_init (struct grid_xbus *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint)
{
    grid_sockbase_init (&self->sockbase, vfptr, hint);
    grid_dist_init (&self->outpipes);
    grid_fq_init (&self->inpipes);
}

void grid_xbus_term (struct grid_xbus *self)
{
    grid_fq_term (&self->inpipes);
    grid_dist_term (&self->outpipes);
    grid_sockbase_term (&self->sockbase);
}

static void grid_xbus_destroy (struct grid_sockbase *self)
{
    struct grid_xbus *xbus;

    xbus = grid_cont (self, struct grid_xbus, sockbase);

    grid_xbus_term (xbus);
    grid_free (xbus);
}

int grid_xbus_add (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xbus *xbus;
    struct grid_xbus_data *data;
    int rcvprio;
    size_t sz;

    xbus = grid_cont (self, struct grid_xbus, sockbase);

    sz = sizeof (rcvprio);
    grid_pipe_getopt (pipe, GRID_SOL_SOCKET, GRID_RCVPRIO, &rcvprio, &sz);
    grid_assert (sz == sizeof (rcvprio));
    grid_assert (rcvprio >= 1 && rcvprio <= 16);

    data = grid_alloc (sizeof (struct grid_xbus_data), "pipe data (xbus)");
    alloc_assert (data);
    grid_fq_add (&xbus->inpipes, &data->initem, pipe, rcvprio);
    grid_dist_add (&xbus->outpipes, &data->outitem, pipe);
    grid_pipe_setdata (pipe, data);

    return 0;
}

void grid_xbus_rm (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xbus *xbus;
    struct grid_xbus_data *data;

    xbus = grid_cont (self, struct grid_xbus, sockbase);
    data = grid_pipe_getdata (pipe);

    grid_fq_rm (&xbus->inpipes, &data->initem);
    grid_dist_rm (&xbus->outpipes, &data->outitem);

    grid_free (data);
}

void grid_xbus_in (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xbus *xbus;
    struct grid_xbus_data *data;

    xbus = grid_cont (self, struct grid_xbus, sockbase);
    data = grid_pipe_getdata (pipe);

    grid_fq_in (&xbus->inpipes, &data->initem);
}

void grid_xbus_out (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xbus *xbus;
    struct grid_xbus_data *data;

    xbus = grid_cont (self, struct grid_xbus, sockbase);
    data = grid_pipe_getdata (pipe);

    grid_dist_out (&xbus->outpipes, &data->outitem);
}

int grid_xbus_events (struct grid_sockbase *self)
{
    return (grid_fq_can_recv (&grid_cont (self, struct grid_xbus,
        sockbase)->inpipes) ? GRID_SOCKBASE_EVENT_IN : 0) | GRID_SOCKBASE_EVENT_OUT;
}

int grid_xbus_send (struct grid_sockbase *self, struct grid_msg *msg)
{
    size_t hdrsz;
    struct grid_pipe *exclude;

    hdrsz = grid_chunkref_size (&msg->sphdr);
    if (hdrsz == 0)
        exclude = NULL;
    else if (hdrsz == sizeof (uint64_t)) {
        memcpy (&exclude, grid_chunkref_data (&msg->sphdr), sizeof (exclude));
        grid_chunkref_term (&msg->sphdr);
        grid_chunkref_init (&msg->sphdr, 0);
    }
    else
        return -EINVAL;

    return grid_dist_send (&grid_cont (self, struct grid_xbus, sockbase)->outpipes,
        msg, exclude);
}

int grid_xbus_recv (struct grid_sockbase *self, struct grid_msg *msg)
{
    int rc;
    struct grid_xbus *xbus;
    struct grid_pipe *pipe;

    xbus = grid_cont (self, struct grid_xbus, sockbase);

    while (1) {

        /*  Get next message in fair-queued manner. */
        rc = grid_fq_recv (&xbus->inpipes, msg, &pipe);
        if (grid_slow (rc < 0))
            return rc;

        /*  The message should have no header. Drop malformed messages. */
        if (grid_chunkref_size (&msg->sphdr) == 0)
            break;
        grid_msg_term (msg);
    }

    /*  Add pipe ID to the message header. */
    grid_chunkref_term (&msg->sphdr);
    grid_chunkref_init (&msg->sphdr, sizeof (uint64_t));
    memset (grid_chunkref_data (&msg->sphdr), 0, sizeof (uint64_t));
    memcpy (grid_chunkref_data (&msg->sphdr), &pipe, sizeof (pipe));

    return 0;
}

int grid_xbus_setopt (GRID_UNUSED struct grid_sockbase *self, GRID_UNUSED int level,
    GRID_UNUSED int option,
    GRID_UNUSED const void *optval, GRID_UNUSED size_t optvallen)
{
    return -ENOPROTOOPT;
}

int grid_xbus_getopt (GRID_UNUSED struct grid_sockbase *self, GRID_UNUSED int level,
    GRID_UNUSED int option,
    GRID_UNUSED void *optval, GRID_UNUSED size_t *optvallen)
{
    return -ENOPROTOOPT;
}

static int grid_xbus_create (void *hint, struct grid_sockbase **sockbase)
{
    struct grid_xbus *self;

    self = grid_alloc (sizeof (struct grid_xbus), "socket (bus)");
    alloc_assert (self);
    grid_xbus_init (self, &grid_xbus_sockbase_vfptr, hint);
    *sockbase = &self->sockbase;

    return 0;
}

int grid_xbus_ispeer (int socktype)
{
    return socktype == GRID_BUS ? 1 : 0;
}

static struct grid_socktype grid_xbus_socktype_struct = {
    AF_SP_RAW,
    GRID_BUS,
    0,
    grid_xbus_create,
    grid_xbus_ispeer,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_socktype *grid_xbus_socktype = &grid_xbus_socktype_struct;


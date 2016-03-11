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

#include "xsurveyor.h"

#include "../../grid.h"
#include "../../survey.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/list.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"
#include "../../utils/attr.h"

#include <stddef.h>

/*  Private functions. */
static void grid_xsurveyor_destroy (struct grid_sockbase *self);

/*  Implementation of grid_sockbase's virtual functions. */
static const struct grid_sockbase_vfptr grid_xsurveyor_sockbase_vfptr = {
    NULL,
    grid_xsurveyor_destroy,
    grid_xsurveyor_add,
    grid_xsurveyor_rm,
    grid_xsurveyor_in,
    grid_xsurveyor_out,
    grid_xsurveyor_events,
    grid_xsurveyor_send,
    grid_xsurveyor_recv,
    grid_xsurveyor_setopt,
    grid_xsurveyor_getopt
};

void grid_xsurveyor_init (struct grid_xsurveyor *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint)
{
    grid_sockbase_init (&self->sockbase, vfptr, hint);
    grid_dist_init (&self->outpipes);
    grid_fq_init (&self->inpipes);
}

void grid_xsurveyor_term (struct grid_xsurveyor *self)
{
    grid_fq_term (&self->inpipes);
    grid_dist_term (&self->outpipes);
    grid_sockbase_term (&self->sockbase);
}

static void grid_xsurveyor_destroy (struct grid_sockbase *self)
{
    struct grid_xsurveyor *xsurveyor;

    xsurveyor = grid_cont (self, struct grid_xsurveyor, sockbase);

    grid_xsurveyor_term (xsurveyor);
    grid_free (xsurveyor);
}

int grid_xsurveyor_add (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xsurveyor *xsurveyor;
    struct grid_xsurveyor_data *data;
    int rcvprio;
    size_t sz;

    xsurveyor = grid_cont (self, struct grid_xsurveyor, sockbase);

    sz = sizeof (rcvprio);
    grid_pipe_getopt (pipe, GRID_SOL_SOCKET, GRID_RCVPRIO, &rcvprio, &sz);
    grid_assert (sz == sizeof (rcvprio));
    grid_assert (rcvprio >= 1 && rcvprio <= 16);

    data = grid_alloc (sizeof (struct grid_xsurveyor_data),
        "pipe data (xsurveyor)");
    alloc_assert (data);
    data->pipe = pipe;
    grid_fq_add (&xsurveyor->inpipes, &data->initem, pipe, rcvprio);
    grid_dist_add (&xsurveyor->outpipes, &data->outitem, pipe);
    grid_pipe_setdata (pipe, data);

    return 0;
}

void grid_xsurveyor_rm (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xsurveyor *xsurveyor;
    struct grid_xsurveyor_data *data;

    xsurveyor = grid_cont (self, struct grid_xsurveyor, sockbase);
    data = grid_pipe_getdata (pipe);

    grid_fq_rm (&xsurveyor->inpipes, &data->initem);
    grid_dist_rm (&xsurveyor->outpipes, &data->outitem);

    grid_free (data);
}

void grid_xsurveyor_in (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xsurveyor *xsurveyor;
    struct grid_xsurveyor_data *data;

    xsurveyor = grid_cont (self, struct grid_xsurveyor, sockbase);
    data = grid_pipe_getdata (pipe);

    grid_fq_in (&xsurveyor->inpipes, &data->initem);
}

void grid_xsurveyor_out (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xsurveyor *xsurveyor;
    struct grid_xsurveyor_data *data;

    xsurveyor = grid_cont (self, struct grid_xsurveyor, sockbase);
    data = grid_pipe_getdata (pipe);

    grid_dist_out (&xsurveyor->outpipes, &data->outitem);
}

int grid_xsurveyor_events (struct grid_sockbase *self)
{
    struct grid_xsurveyor *xsurveyor;
    int events;

    xsurveyor = grid_cont (self, struct grid_xsurveyor, sockbase);

    events = GRID_SOCKBASE_EVENT_OUT;
    if (grid_fq_can_recv (&xsurveyor->inpipes))
        events |= GRID_SOCKBASE_EVENT_IN;
    return events;
}

int grid_xsurveyor_send (struct grid_sockbase *self, struct grid_msg *msg)
{
    return grid_dist_send (
        &grid_cont (self, struct grid_xsurveyor, sockbase)->outpipes, msg, NULL);
}

int grid_xsurveyor_recv (struct grid_sockbase *self, struct grid_msg *msg)
{
    int rc;
    struct grid_xsurveyor *xsurveyor;

    xsurveyor = grid_cont (self, struct grid_xsurveyor, sockbase);

    rc = grid_fq_recv (&xsurveyor->inpipes, msg, NULL);
    if (grid_slow (rc < 0))
        return rc;

    /*  Split the header from the body, if needed. */
    if (!(rc & GRID_PIPE_PARSED)) {
        if (grid_slow (grid_chunkref_size (&msg->body) < sizeof (uint32_t))) {
            grid_msg_term (msg);
            return -EAGAIN;
        }
        grid_assert (grid_chunkref_size (&msg->sphdr) == 0);
        grid_chunkref_term (&msg->sphdr);
        grid_chunkref_init (&msg->sphdr, sizeof (uint32_t));
        memcpy (grid_chunkref_data (&msg->sphdr), grid_chunkref_data (&msg->body),
           sizeof (uint32_t));
        grid_chunkref_trim (&msg->body, sizeof (uint32_t));
    }

    return 0;
}

int grid_xsurveyor_setopt (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED int level, GRID_UNUSED int option,
    GRID_UNUSED const void *optval, GRID_UNUSED size_t optvallen)
{
    return -ENOPROTOOPT;
}

int grid_xsurveyor_getopt (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED int level, GRID_UNUSED int option,
    GRID_UNUSED void *optval, GRID_UNUSED size_t *optvallen)
{
    return -ENOPROTOOPT;
}

static int grid_xsurveyor_create (void *hint, struct grid_sockbase **sockbase)
{
    struct grid_xsurveyor *self;

    self = grid_alloc (sizeof (struct grid_xsurveyor), "socket (xsurveyor)");
    alloc_assert (self);
    grid_xsurveyor_init (self, &grid_xsurveyor_sockbase_vfptr, hint);
    *sockbase = &self->sockbase;

    return 0;
}

int grid_xsurveyor_ispeer (int socktype)
{
    return socktype == GRID_RESPONDENT ? 1 : 0;
}

static struct grid_socktype grid_xsurveyor_socktype_struct = {
    AF_SP_RAW,
    GRID_SURVEYOR,
    0,
    grid_xsurveyor_create,
    grid_xsurveyor_ispeer,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_socktype *grid_xsurveyor_socktype = &grid_xsurveyor_socktype_struct;


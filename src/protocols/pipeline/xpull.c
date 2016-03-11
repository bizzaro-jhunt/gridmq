/*
    Copyright (c) 2012-2014 Martin Sustrik  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.

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

#include "xpull.h"

#include "../../grid.h"
#include "../../pipeline.h"

#include "../utils/fq.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"
#include "../../utils/attr.h"

struct grid_xpull_data {
    struct grid_fq_data fq;
};

struct grid_xpull {
    struct grid_sockbase sockbase;
    struct grid_fq fq;
};

/*  Private functions. */
static void grid_xpull_init (struct grid_xpull *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint);
static void grid_xpull_term (struct grid_xpull *self);

/*  Implementation of grid_sockbase's virtual functions. */
static void grid_xpull_destroy (struct grid_sockbase *self);
static int grid_xpull_add (struct grid_sockbase *self, struct grid_pipe *pipe);
static void grid_xpull_rm (struct grid_sockbase *self, struct grid_pipe *pipe);
static void grid_xpull_in (struct grid_sockbase *self, struct grid_pipe *pipe);
static void grid_xpull_out (struct grid_sockbase *self, struct grid_pipe *pipe);
static int grid_xpull_events (struct grid_sockbase *self);
static int grid_xpull_recv (struct grid_sockbase *self, struct grid_msg *msg);
static int grid_xpull_setopt (struct grid_sockbase *self, int level, int option,
    const void *optval, size_t optvallen);
static int grid_xpull_getopt (struct grid_sockbase *self, int level, int option,
    void *optval, size_t *optvallen);
static const struct grid_sockbase_vfptr grid_xpull_sockbase_vfptr = {
    NULL,
    grid_xpull_destroy,
    grid_xpull_add,
    grid_xpull_rm,
    grid_xpull_in,
    grid_xpull_out,
    grid_xpull_events,
    NULL,
    grid_xpull_recv,
    grid_xpull_setopt,
    grid_xpull_getopt
};

static void grid_xpull_init (struct grid_xpull *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint)
{
    grid_sockbase_init (&self->sockbase, vfptr, hint);
    grid_fq_init (&self->fq);
}

static void grid_xpull_term (struct grid_xpull *self)
{
    grid_fq_term (&self->fq);
    grid_sockbase_term (&self->sockbase);
}

void grid_xpull_destroy (struct grid_sockbase *self)
{
    struct grid_xpull *xpull;

    xpull = grid_cont (self, struct grid_xpull, sockbase);

    grid_xpull_term (xpull);
    grid_free (xpull);
}

static int grid_xpull_add (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xpull *xpull;
    struct grid_xpull_data *data;
    int rcvprio;
    size_t sz;

    xpull = grid_cont (self, struct grid_xpull, sockbase);

    sz = sizeof (rcvprio);
    grid_pipe_getopt (pipe, GRID_SOL_SOCKET, GRID_RCVPRIO, &rcvprio, &sz);
    grid_assert (sz == sizeof (rcvprio));
    grid_assert (rcvprio >= 1 && rcvprio <= 16);

    data = grid_alloc (sizeof (struct grid_xpull_data), "pipe data (pull)");
    alloc_assert (data);
    grid_pipe_setdata (pipe, data);
    grid_fq_add (&xpull->fq, &data->fq, pipe, rcvprio);

    return 0;
}

static void grid_xpull_rm (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xpull *xpull;
    struct grid_xpull_data *data;

    xpull = grid_cont (self, struct grid_xpull, sockbase);
    data = grid_pipe_getdata (pipe);
    grid_fq_rm (&xpull->fq, &data->fq);
    grid_free (data);
}

static void grid_xpull_in (GRID_UNUSED struct grid_sockbase *self,
                         struct grid_pipe *pipe)
{
    struct grid_xpull *xpull;
    struct grid_xpull_data *data;

    xpull = grid_cont (self, struct grid_xpull, sockbase);
    data = grid_pipe_getdata (pipe);
    grid_fq_in (&xpull->fq, &data->fq);
}

static void grid_xpull_out (GRID_UNUSED struct grid_sockbase *self,
                          GRID_UNUSED struct grid_pipe *pipe)
{
    /*  We are not going to send any messages, so there's no point is
        maintaining a list of pipes ready for sending. */
}

static int grid_xpull_events (struct grid_sockbase *self)
{
    return grid_fq_can_recv (&grid_cont (self, struct grid_xpull, sockbase)->fq) ?
        GRID_SOCKBASE_EVENT_IN : 0;
}

static int grid_xpull_recv (struct grid_sockbase *self, struct grid_msg *msg)
{
    int rc;

    rc = grid_fq_recv (&grid_cont (self, struct grid_xpull, sockbase)->fq,
         msg, NULL);

    /*  Discard GRID_PIPEBASE_PARSED flag. */
    return rc < 0 ? rc : 0;
}

static int grid_xpull_setopt (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED int level, GRID_UNUSED int option,
    GRID_UNUSED const void *optval, GRID_UNUSED size_t optvallen)
{
    return -ENOPROTOOPT;
}

static int grid_xpull_getopt (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED int level, GRID_UNUSED int option,
    GRID_UNUSED void *optval, GRID_UNUSED size_t *optvallen)
{
    return -ENOPROTOOPT;
}

int grid_xpull_create (void *hint, struct grid_sockbase **sockbase)
{
    struct grid_xpull *self;

    self = grid_alloc (sizeof (struct grid_xpull), "socket (pull)");
    alloc_assert (self);
    grid_xpull_init (self, &grid_xpull_sockbase_vfptr, hint);
    *sockbase = &self->sockbase;

    return 0;
}

int grid_xpull_ispeer (int socktype)
{
    return socktype == GRID_PUSH ? 1 : 0;
}

static struct grid_socktype grid_xpull_socktype_struct = {
    AF_SP_RAW,
    GRID_PULL,
    GRID_SOCKTYPE_FLAG_NOSEND,
    grid_xpull_create,
    grid_xpull_ispeer,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_socktype *grid_xpull_socktype = &grid_xpull_socktype_struct;


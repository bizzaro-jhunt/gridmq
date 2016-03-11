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

#include "xpush.h"

#include "../../grid.h"
#include "../../pipeline.h"

#include "../utils/lb.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"
#include "../../utils/attr.h"

struct grid_xpush_data {
    struct grid_lb_data lb;
};

struct grid_xpush {
    struct grid_sockbase sockbase;
    struct grid_lb lb;
};

/*  Private functions. */
static void grid_xpush_init (struct grid_xpush *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint);
static void grid_xpush_term (struct grid_xpush *self);

/*  Implementation of grid_sockbase's virtual functions. */
static void grid_xpush_destroy (struct grid_sockbase *self);
static int grid_xpush_add (struct grid_sockbase *self, struct grid_pipe *pipe);
static void grid_xpush_rm (struct grid_sockbase *self, struct grid_pipe *pipe);
static void grid_xpush_in (struct grid_sockbase *self, struct grid_pipe *pipe);
static void grid_xpush_out (struct grid_sockbase *self, struct grid_pipe *pipe);
static int grid_xpush_events (struct grid_sockbase *self);
static int grid_xpush_send (struct grid_sockbase *self, struct grid_msg *msg);
static int grid_xpush_setopt (struct grid_sockbase *self, int level, int option,
    const void *optval, size_t optvallen);
static int grid_xpush_getopt (struct grid_sockbase *self, int level, int option,
    void *optval, size_t *optvallen);
static const struct grid_sockbase_vfptr grid_xpush_sockbase_vfptr = {
    NULL,
    grid_xpush_destroy,
    grid_xpush_add,
    grid_xpush_rm,
    grid_xpush_in,
    grid_xpush_out,
    grid_xpush_events,
    grid_xpush_send,
    NULL,
    grid_xpush_setopt,
    grid_xpush_getopt
};

static void grid_xpush_init (struct grid_xpush *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint)
{
    grid_sockbase_init (&self->sockbase, vfptr, hint);
    grid_lb_init (&self->lb);
}

static void grid_xpush_term (struct grid_xpush *self)
{
    grid_lb_term (&self->lb);
    grid_sockbase_term (&self->sockbase);
}

void grid_xpush_destroy (struct grid_sockbase *self)
{
    struct grid_xpush *xpush;

    xpush = grid_cont (self, struct grid_xpush, sockbase);

    grid_xpush_term (xpush);
    grid_free (xpush);
}

static int grid_xpush_add (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xpush *xpush;
    struct grid_xpush_data *data;
    int sndprio;
    size_t sz;

    xpush = grid_cont (self, struct grid_xpush, sockbase);

    sz = sizeof (sndprio);
    grid_pipe_getopt (pipe, GRID_SOL_SOCKET, GRID_SNDPRIO, &sndprio, &sz);
    grid_assert (sz == sizeof (sndprio));
    grid_assert (sndprio >= 1 && sndprio <= 16);

    data = grid_alloc (sizeof (struct grid_xpush_data), "pipe data (push)");
    alloc_assert (data);
    grid_pipe_setdata (pipe, data);
    grid_lb_add (&xpush->lb, &data->lb, pipe, sndprio);

    return 0;
}

static void grid_xpush_rm (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xpush *xpush;
    struct grid_xpush_data *data;

    xpush = grid_cont (self, struct grid_xpush, sockbase);
    data = grid_pipe_getdata (pipe);
    grid_lb_rm (&xpush->lb, &data->lb);
    grid_free (data);

    grid_sockbase_stat_increment (self, GRID_STAT_CURRENT_SND_PRIORITY,
        grid_lb_get_priority (&xpush->lb));
}

static void grid_xpush_in (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED struct grid_pipe *pipe)
{
    /*  We are not going to receive any messages, so there's no need to store
        the list of inbound pipes. */
}

static void grid_xpush_out (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xpush *xpush;
    struct grid_xpush_data *data;

    xpush = grid_cont (self, struct grid_xpush, sockbase);
    data = grid_pipe_getdata (pipe);
    grid_lb_out (&xpush->lb, &data->lb);
    grid_sockbase_stat_increment (self, GRID_STAT_CURRENT_SND_PRIORITY,
        grid_lb_get_priority (&xpush->lb));
}

static int grid_xpush_events (struct grid_sockbase *self)
{
    return grid_lb_can_send (&grid_cont (self, struct grid_xpush, sockbase)->lb) ?
        GRID_SOCKBASE_EVENT_OUT : 0;
}

static int grid_xpush_send (struct grid_sockbase *self, struct grid_msg *msg)
{
    return grid_lb_send (&grid_cont (self, struct grid_xpush, sockbase)->lb,
        msg, NULL);
}

static int grid_xpush_setopt (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED int level, GRID_UNUSED int option,
    GRID_UNUSED const void *optval, GRID_UNUSED size_t optvallen)
{
    return -ENOPROTOOPT;
}

static int grid_xpush_getopt (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED int level, GRID_UNUSED int option,
    GRID_UNUSED void *optval, GRID_UNUSED size_t *optvallen)
{
    return -ENOPROTOOPT;
}

int grid_xpush_create (void *hint, struct grid_sockbase **sockbase)
{
    struct grid_xpush *self;

    self = grid_alloc (sizeof (struct grid_xpush), "socket (push)");
    alloc_assert (self);
    grid_xpush_init (self, &grid_xpush_sockbase_vfptr, hint);
    *sockbase = &self->sockbase;

    return 0;
}

int grid_xpush_ispeer (int socktype)
{
    return socktype == GRID_PULL ? 1 : 0;
}

static struct grid_socktype grid_xpush_socktype_struct = {
    AF_SP_RAW,
    GRID_PUSH,
    GRID_SOCKTYPE_FLAG_NORECV,
    grid_xpush_create,
    grid_xpush_ispeer,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_socktype *grid_xpush_socktype = &grid_xpush_socktype_struct;


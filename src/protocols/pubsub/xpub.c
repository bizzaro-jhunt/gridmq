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

#include "xpub.h"

#include "../../grid.h"
#include "../../pubsub.h"

#include "../utils/dist.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"
#include "../../utils/attr.h"

#include <stddef.h>

struct grid_xpub_data {
    struct grid_dist_data item;
};

struct grid_xpub {

    /*  The generic socket base class. */
    struct grid_sockbase sockbase;

    /*  Distributor. */
    struct grid_dist outpipes;
};

/*  Private functions. */
static void grid_xpub_init (struct grid_xpub *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint);
static void grid_xpub_term (struct grid_xpub *self);

/*  Implementation of grid_sockbase's virtual functions. */
static void grid_xpub_destroy (struct grid_sockbase *self);
static int grid_xpub_add (struct grid_sockbase *self, struct grid_pipe *pipe);
static void grid_xpub_rm (struct grid_sockbase *self, struct grid_pipe *pipe);
static void grid_xpub_in (struct grid_sockbase *self, struct grid_pipe *pipe);
static void grid_xpub_out (struct grid_sockbase *self, struct grid_pipe *pipe);
static int grid_xpub_events (struct grid_sockbase *self);
static int grid_xpub_send (struct grid_sockbase *self, struct grid_msg *msg);
static int grid_xpub_setopt (struct grid_sockbase *self, int level, int option,
    const void *optval, size_t optvallen);
static int grid_xpub_getopt (struct grid_sockbase *self, int level, int option,
    void *optval, size_t *optvallen);
static const struct grid_sockbase_vfptr grid_xpub_sockbase_vfptr = {
    NULL,
    grid_xpub_destroy,
    grid_xpub_add,
    grid_xpub_rm,
    grid_xpub_in,
    grid_xpub_out,
    grid_xpub_events,
    grid_xpub_send,
    NULL,
    grid_xpub_setopt,
    grid_xpub_getopt
};

static void grid_xpub_init (struct grid_xpub *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint)
{
    grid_sockbase_init (&self->sockbase, vfptr, hint);
    grid_dist_init (&self->outpipes);
}

static void grid_xpub_term (struct grid_xpub *self)
{
    grid_dist_term (&self->outpipes);
    grid_sockbase_term (&self->sockbase);
}

void grid_xpub_destroy (struct grid_sockbase *self)
{
    struct grid_xpub *xpub;

    xpub = grid_cont (self, struct grid_xpub, sockbase);

    grid_xpub_term (xpub);
    grid_free (xpub);
}

static int grid_xpub_add (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xpub *xpub;
    struct grid_xpub_data *data;

    xpub = grid_cont (self, struct grid_xpub, sockbase);

    data = grid_alloc (sizeof (struct grid_xpub_data), "pipe data (pub)");
    alloc_assert (data);
    grid_dist_add (&xpub->outpipes, &data->item, pipe);
    grid_pipe_setdata (pipe, data);

    return 0;
}

static void grid_xpub_rm (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xpub *xpub;
    struct grid_xpub_data *data;

    xpub = grid_cont (self, struct grid_xpub, sockbase);
    data = grid_pipe_getdata (pipe);

    grid_dist_rm (&xpub->outpipes, &data->item);

    grid_free (data);
}

static void grid_xpub_in (GRID_UNUSED struct grid_sockbase *self,
                       GRID_UNUSED struct grid_pipe *pipe)
{
    /*  We shouldn't get any messages from subscribers. */
    grid_assert (0);
}

static void grid_xpub_out (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xpub *xpub;
    struct grid_xpub_data *data;

    xpub = grid_cont (self, struct grid_xpub, sockbase);
    data = grid_pipe_getdata (pipe);

    grid_dist_out (&xpub->outpipes, &data->item);
}

static int grid_xpub_events (GRID_UNUSED struct grid_sockbase *self)
{
    return GRID_SOCKBASE_EVENT_OUT;
}

static int grid_xpub_send (struct grid_sockbase *self, struct grid_msg *msg)
{
    return grid_dist_send (&grid_cont (self, struct grid_xpub, sockbase)->outpipes,
        msg, NULL);
}

static int grid_xpub_setopt (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED int level, GRID_UNUSED int option,
    GRID_UNUSED const void *optval, GRID_UNUSED size_t optvallen)
{
    return -ENOPROTOOPT;
}

static int grid_xpub_getopt (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED int level, GRID_UNUSED int option,
    GRID_UNUSED void *optval, GRID_UNUSED size_t *optvallen)
{
    return -ENOPROTOOPT;
}

int grid_xpub_create (void *hint, struct grid_sockbase **sockbase)
{
    struct grid_xpub *self;

    self = grid_alloc (sizeof (struct grid_xpub), "socket (xpub)");
    alloc_assert (self);
    grid_xpub_init (self, &grid_xpub_sockbase_vfptr, hint);
    *sockbase = &self->sockbase;

    return 0;
}

int grid_xpub_ispeer (int socktype)
{
     return socktype == GRID_SUB ? 1 : 0;
}

static struct grid_socktype grid_xpub_socktype_struct = {
    AF_SP_RAW,
    GRID_PUB,
    GRID_SOCKTYPE_FLAG_NORECV,
    grid_xpub_create,
    grid_xpub_ispeer,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_socktype *grid_xpub_socktype = &grid_xpub_socktype_struct;


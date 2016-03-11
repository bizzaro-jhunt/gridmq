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

#include "xpair.h"

#include "../../grid.h"
#include "../../pair.h"

#include "../utils/excl.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"
#include "../../utils/attr.h"

struct grid_xpair {
    struct grid_sockbase sockbase;
    struct grid_excl excl;
};

/*  Private functions. */
static void grid_xpair_init (struct grid_xpair *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint);
static void grid_xpair_term (struct grid_xpair *self);

/*  Implementation of grid_sockbase's virtual functions. */
static void grid_xpair_destroy (struct grid_sockbase *self);
static int grid_xpair_add (struct grid_sockbase *self, struct grid_pipe *pipe);
static void grid_xpair_rm (struct grid_sockbase *self, struct grid_pipe *pipe);
static void grid_xpair_in (struct grid_sockbase *self, struct grid_pipe *pipe);
static void grid_xpair_out (struct grid_sockbase *self, struct grid_pipe *pipe);
static int grid_xpair_events (struct grid_sockbase *self);
static int grid_xpair_send (struct grid_sockbase *self, struct grid_msg *msg);
static int grid_xpair_recv (struct grid_sockbase *self, struct grid_msg *msg);
static int grid_xpair_setopt (struct grid_sockbase *self, int level, int option,
        const void *optval, size_t optvallen);
static int grid_xpair_getopt (struct grid_sockbase *self, int level, int option,
        void *optval, size_t *optvallen);
static const struct grid_sockbase_vfptr grid_xpair_sockbase_vfptr = {
    NULL,
    grid_xpair_destroy,
    grid_xpair_add,
    grid_xpair_rm,
    grid_xpair_in,
    grid_xpair_out,
    grid_xpair_events,
    grid_xpair_send,
    grid_xpair_recv,
    grid_xpair_setopt,
    grid_xpair_getopt
};

static void grid_xpair_init (struct grid_xpair *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint)
{
    grid_sockbase_init (&self->sockbase, vfptr, hint);
    grid_excl_init (&self->excl);
}

static void grid_xpair_term (struct grid_xpair *self)
{
    grid_excl_term (&self->excl);
    grid_sockbase_term (&self->sockbase);
}

void grid_xpair_destroy (struct grid_sockbase *self)
{
    struct grid_xpair *xpair;

    xpair = grid_cont (self, struct grid_xpair, sockbase);

    grid_xpair_term (xpair);
    grid_free (xpair);
}

static int grid_xpair_add (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    return grid_excl_add (&grid_cont (self, struct grid_xpair, sockbase)->excl,
        pipe);
}

static void grid_xpair_rm (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    grid_excl_rm (&grid_cont (self, struct grid_xpair, sockbase)->excl, pipe);
}

static void grid_xpair_in (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    grid_excl_in (&grid_cont (self, struct grid_xpair, sockbase)->excl, pipe);
}

static void grid_xpair_out (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    grid_excl_out (&grid_cont (self, struct grid_xpair, sockbase)->excl, pipe);
}

static int grid_xpair_events (struct grid_sockbase *self)
{
    struct grid_xpair *xpair;
    int events;

    xpair = grid_cont (self, struct grid_xpair, sockbase);

    events = 0;
    if (grid_excl_can_recv (&xpair->excl))
        events |= GRID_SOCKBASE_EVENT_IN;
    if (grid_excl_can_send (&xpair->excl))
        events |= GRID_SOCKBASE_EVENT_OUT;
    return events;
}

static int grid_xpair_send (struct grid_sockbase *self, struct grid_msg *msg)
{
    return grid_excl_send (&grid_cont (self, struct grid_xpair, sockbase)->excl,
        msg);
}

static int grid_xpair_recv (struct grid_sockbase *self, struct grid_msg *msg)
{
    int rc;

    rc = grid_excl_recv (&grid_cont (self, struct grid_xpair, sockbase)->excl, msg);

    /*  Discard GRID_PIPEBASE_PARSED flag. */
    return rc < 0 ? rc : 0;
}

static int grid_xpair_setopt (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED int level, GRID_UNUSED int option,
    GRID_UNUSED const void *optval, GRID_UNUSED size_t optvallen)
{
    return -ENOPROTOOPT;
}

static int grid_xpair_getopt (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED int level, GRID_UNUSED int option,
    GRID_UNUSED void *optval, GRID_UNUSED size_t *optvallen)
{
    return -ENOPROTOOPT;
}

int grid_xpair_create (void *hint, struct grid_sockbase **sockbase)
{
    struct grid_xpair *self;

    self = grid_alloc (sizeof (struct grid_xpair), "socket (pair)");
    alloc_assert (self);
    grid_xpair_init (self, &grid_xpair_sockbase_vfptr, hint);
    *sockbase = &self->sockbase;

    return 0;
}

int grid_xpair_ispeer (int socktype)
{
    return socktype == GRID_PAIR ? 1 : 0;
}

static struct grid_socktype grid_xpair_socktype_struct = {
    AF_SP_RAW,
    GRID_PAIR,
    0,
    grid_xpair_create,
    grid_xpair_ispeer,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_socktype *grid_xpair_socktype = &grid_xpair_socktype_struct;


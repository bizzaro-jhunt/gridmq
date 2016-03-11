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

#include "xsub.h"
#include "trie.h"

#include "../../grid.h"
#include "../../pubsub.h"

#include "../utils/fq.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"
#include "../../utils/attr.h"

struct grid_xsub_data {
    struct grid_fq_data fq;
};

struct grid_xsub {
    struct grid_sockbase sockbase;
    struct grid_fq fq;
    struct grid_trie trie;
};

/*  Private functions. */
static void grid_xsub_init (struct grid_xsub *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint);
static void grid_xsub_term (struct grid_xsub *self);

/*  Implementation of grid_sockbase's virtual functions. */
static void grid_xsub_destroy (struct grid_sockbase *self);
static int grid_xsub_add (struct grid_sockbase *self, struct grid_pipe *pipe);
static void grid_xsub_rm (struct grid_sockbase *self, struct grid_pipe *pipe);
static void grid_xsub_in (struct grid_sockbase *self, struct grid_pipe *pipe);
static void grid_xsub_out (struct grid_sockbase *self, struct grid_pipe *pipe);
static int grid_xsub_events (struct grid_sockbase *self);
static int grid_xsub_recv (struct grid_sockbase *self, struct grid_msg *msg);
static int grid_xsub_setopt (struct grid_sockbase *self, int level, int option,
    const void *optval, size_t optvallen);
static int grid_xsub_getopt (struct grid_sockbase *self, int level, int option,
    void *optval, size_t *optvallen);
static const struct grid_sockbase_vfptr grid_xsub_sockbase_vfptr = {
    NULL,
    grid_xsub_destroy,
    grid_xsub_add,
    grid_xsub_rm,
    grid_xsub_in,
    grid_xsub_out,
    grid_xsub_events,
    NULL,
    grid_xsub_recv,
    grid_xsub_setopt,
    grid_xsub_getopt
};

static void grid_xsub_init (struct grid_xsub *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint)
{
    grid_sockbase_init (&self->sockbase, vfptr, hint);
    grid_fq_init (&self->fq);
    grid_trie_init (&self->trie);
}

static void grid_xsub_term (struct grid_xsub *self)
{
    grid_trie_term (&self->trie);
    grid_fq_term (&self->fq);
    grid_sockbase_term (&self->sockbase);
}

void grid_xsub_destroy (struct grid_sockbase *self)
{
    struct grid_xsub *xsub;

    xsub = grid_cont (self, struct grid_xsub, sockbase);

    grid_xsub_term (xsub);
    grid_free (xsub);
}

static int grid_xsub_add (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xsub *xsub;
    struct grid_xsub_data *data;
    int rcvprio;
    size_t sz;

    xsub = grid_cont (self, struct grid_xsub, sockbase);

    sz = sizeof (rcvprio);
    grid_pipe_getopt (pipe, GRID_SOL_SOCKET, GRID_RCVPRIO, &rcvprio, &sz);
    grid_assert (sz == sizeof (rcvprio));
    grid_assert (rcvprio >= 1 && rcvprio <= 16);

    data = grid_alloc (sizeof (struct grid_xsub_data), "pipe data (sub)");
    alloc_assert (data);
    grid_pipe_setdata (pipe, data);
    grid_fq_add (&xsub->fq, &data->fq, pipe, rcvprio);

    return 0;
}

static void grid_xsub_rm (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xsub *xsub;
    struct grid_xsub_data *data;

    xsub = grid_cont (self, struct grid_xsub, sockbase);
    data = grid_pipe_getdata (pipe);
    grid_fq_rm (&xsub->fq, &data->fq);
    grid_free (data);
}

static void grid_xsub_in (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xsub *xsub;
    struct grid_xsub_data *data;

    xsub = grid_cont (self, struct grid_xsub, sockbase);
    data = grid_pipe_getdata (pipe);
    grid_fq_in (&xsub->fq, &data->fq);
}

static void grid_xsub_out (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED struct grid_pipe *pipe)
{
    /*  We are not going to send any messages until subscription forwarding
        is implemented, so there's no point is maintaining a list of pipes
        ready for sending. */
}

static int grid_xsub_events (struct grid_sockbase *self)
{
    return grid_fq_can_recv (&grid_cont (self, struct grid_xsub, sockbase)->fq) ?
        GRID_SOCKBASE_EVENT_IN : 0;
}

static int grid_xsub_recv (struct grid_sockbase *self, struct grid_msg *msg)
{
    int rc;
    struct grid_xsub *xsub;

    xsub = grid_cont (self, struct grid_xsub, sockbase);

    /*  Loop while a matching message is found or when there are no more
        messages to receive. */
    while (1) {
        rc = grid_fq_recv (&xsub->fq, msg, NULL);
        if (grid_slow (rc == -EAGAIN))
            return -EAGAIN;
        errnum_assert (rc >= 0, -rc);
        rc = grid_trie_match (&xsub->trie, grid_chunkref_data (&msg->body),
            grid_chunkref_size (&msg->body));
        if (rc == 0) {
            grid_msg_term (msg);
            continue;
        }
        if (rc == 1)
            return 0;
        errnum_assert (0, -rc);
    }
}

static int grid_xsub_setopt (struct grid_sockbase *self, int level, int option,
        const void *optval, size_t optvallen)
{
    int rc;
    struct grid_xsub *xsub;

    xsub = grid_cont (self, struct grid_xsub, sockbase);

    if (level != GRID_SUB)
        return -ENOPROTOOPT;

    if (option == GRID_SUB_SUBSCRIBE) {
        rc = grid_trie_subscribe (&xsub->trie, optval, optvallen);
        if (rc >= 0)
            return 0;
        return rc;
    }

    if (option == GRID_SUB_UNSUBSCRIBE) {
        rc = grid_trie_unsubscribe (&xsub->trie, optval, optvallen);
        if (rc >= 0)
            return 0;
        return rc;
    }

    return -ENOPROTOOPT;
}

static int grid_xsub_getopt (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED int level, GRID_UNUSED int option,
    GRID_UNUSED void *optval, GRID_UNUSED size_t *optvallen)
{
    return -ENOPROTOOPT;
}

int grid_xsub_create (void *hint, struct grid_sockbase **sockbase)
{
    struct grid_xsub *self;

    self = grid_alloc (sizeof (struct grid_xsub), "socket (xsub)");
    alloc_assert (self);
    grid_xsub_init (self, &grid_xsub_sockbase_vfptr, hint);
    *sockbase = &self->sockbase;

    return 0;
}

int grid_xsub_ispeer (int socktype)
{
    return socktype == GRID_PUB ? 1 : 0;
}

static struct grid_socktype grid_xsub_socktype_struct = {
    AF_SP_RAW,
    GRID_SUB,
    GRID_SOCKTYPE_FLAG_NOSEND,
    grid_xsub_create,
    grid_xsub_ispeer,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_socktype *grid_xsub_socktype = &grid_xsub_socktype_struct;


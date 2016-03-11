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

#include "xreq.h"

#include "../../grid.h"
#include "../../reqrep.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"
#include "../../utils/attr.h"

struct grid_xreq_data {
    struct grid_lb_data lb;
    struct grid_fq_data fq;
};

/*  Private functions. */
static void grid_xreq_destroy (struct grid_sockbase *self);

static const struct grid_sockbase_vfptr grid_xreq_sockbase_vfptr = {
    NULL,
    grid_xreq_destroy,
    grid_xreq_add,
    grid_xreq_rm,
    grid_xreq_in,
    grid_xreq_out,
    grid_xreq_events,
    grid_xreq_send,
    grid_xreq_recv,
    grid_xreq_setopt,
    grid_xreq_getopt
};

void grid_xreq_init (struct grid_xreq *self, const struct grid_sockbase_vfptr *vfptr,
    void *hint)
{
    grid_sockbase_init (&self->sockbase, vfptr, hint);
    grid_lb_init (&self->lb);
    grid_fq_init (&self->fq);
}

void grid_xreq_term (struct grid_xreq *self)
{
    grid_fq_term (&self->fq);
    grid_lb_term (&self->lb);
    grid_sockbase_term (&self->sockbase);
}

static void grid_xreq_destroy (struct grid_sockbase *self)
{
    struct grid_xreq *xreq;

    xreq = grid_cont (self, struct grid_xreq, sockbase);

    grid_xreq_term (xreq);
    grid_free (xreq);
}

int grid_xreq_add (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xreq *xreq;
    struct grid_xreq_data *data;
    int sndprio;
    int rcvprio;
    size_t sz;

    xreq = grid_cont (self, struct grid_xreq, sockbase);

    sz = sizeof (sndprio);
    grid_pipe_getopt (pipe, GRID_SOL_SOCKET, GRID_SNDPRIO, &sndprio, &sz);
    grid_assert (sz == sizeof (sndprio));
    grid_assert (sndprio >= 1 && sndprio <= 16);

    sz = sizeof (rcvprio);
    grid_pipe_getopt (pipe, GRID_SOL_SOCKET, GRID_RCVPRIO, &rcvprio, &sz);
    grid_assert (sz == sizeof (rcvprio));
    grid_assert (rcvprio >= 1 && rcvprio <= 16);

    data = grid_alloc (sizeof (struct grid_xreq_data), "pipe data (req)");
    alloc_assert (data);
    grid_pipe_setdata (pipe, data);
    grid_lb_add (&xreq->lb, &data->lb, pipe, sndprio);
    grid_fq_add (&xreq->fq, &data->fq, pipe, rcvprio);

    return 0;
}

void grid_xreq_rm (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xreq *xreq;
    struct grid_xreq_data *data;

    xreq = grid_cont (self, struct grid_xreq, sockbase);
    data = grid_pipe_getdata (pipe);
    grid_lb_rm (&xreq->lb, &data->lb);
    grid_fq_rm (&xreq->fq, &data->fq);
    grid_free (data);

    grid_sockbase_stat_increment (self, GRID_STAT_CURRENT_SND_PRIORITY,
        grid_lb_get_priority (&xreq->lb));
}

void grid_xreq_in (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xreq *xreq;
    struct grid_xreq_data *data;

    xreq = grid_cont (self, struct grid_xreq, sockbase);
    data = grid_pipe_getdata (pipe);
    grid_fq_in (&xreq->fq, &data->fq);
}

void grid_xreq_out (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_xreq *xreq;
    struct grid_xreq_data *data;

    xreq = grid_cont (self, struct grid_xreq, sockbase);
    data = grid_pipe_getdata (pipe);
    grid_lb_out (&xreq->lb, &data->lb);

    grid_sockbase_stat_increment (self, GRID_STAT_CURRENT_SND_PRIORITY,
        grid_lb_get_priority (&xreq->lb));
}

int grid_xreq_events (struct grid_sockbase *self)
{
    struct grid_xreq *xreq;

    xreq = grid_cont (self, struct grid_xreq, sockbase);

    return (grid_fq_can_recv (&xreq->fq) ? GRID_SOCKBASE_EVENT_IN : 0) |
        (grid_lb_can_send (&xreq->lb) ? GRID_SOCKBASE_EVENT_OUT : 0);
}

int grid_xreq_send (struct grid_sockbase *self, struct grid_msg *msg)
{
    return grid_xreq_send_to (self, msg, NULL);
}

int grid_xreq_send_to (struct grid_sockbase *self, struct grid_msg *msg,
    struct grid_pipe **to)
{
    int rc;

    /*  If request cannot be sent due to the pushback, drop it silenly. */
    rc = grid_lb_send (&grid_cont (self, struct grid_xreq, sockbase)->lb, msg, to);
    if (grid_slow (rc == -EAGAIN))
        return -EAGAIN;
    errnum_assert (rc >= 0, -rc);

    return 0;
}

int grid_xreq_recv (struct grid_sockbase *self, struct grid_msg *msg)
{
    int rc;

    rc = grid_fq_recv (&grid_cont (self, struct grid_xreq, sockbase)->fq, msg, NULL);
    if (rc == -EAGAIN)
        return -EAGAIN;
    errnum_assert (rc >= 0, -rc);

    if (!(rc & GRID_PIPE_PARSED)) {

        /*  Ignore malformed replies. */
        if (grid_slow (grid_chunkref_size (&msg->body) < sizeof (uint32_t))) {
            grid_msg_term (msg);
            return -EAGAIN;
        }

        /*  Split the message into the header and the body. */
        grid_assert (grid_chunkref_size (&msg->sphdr) == 0);
        grid_chunkref_term (&msg->sphdr);
        grid_chunkref_init (&msg->sphdr, sizeof (uint32_t));
        memcpy (grid_chunkref_data (&msg->sphdr), grid_chunkref_data (&msg->body),
            sizeof (uint32_t));
        grid_chunkref_trim (&msg->body, sizeof (uint32_t));
    }

    return 0;
}

int grid_xreq_setopt (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED int level, GRID_UNUSED int option,
    GRID_UNUSED const void *optval, GRID_UNUSED size_t optvallen)
{
    return -ENOPROTOOPT;
}

int grid_xreq_getopt (GRID_UNUSED struct grid_sockbase *self,
    GRID_UNUSED int level, GRID_UNUSED int option,
    GRID_UNUSED void *optval, GRID_UNUSED size_t *optvallen)
{
    return -ENOPROTOOPT;
}

static int grid_xreq_create (void *hint, struct grid_sockbase **sockbase)
{
    struct grid_xreq *self;

    self = grid_alloc (sizeof (struct grid_xreq), "socket (xreq)");
    alloc_assert (self);
    grid_xreq_init (self, &grid_xreq_sockbase_vfptr, hint);
    *sockbase = &self->sockbase;

    return 0;
}

int grid_xreq_ispeer (int socktype)
{
    return socktype == GRID_REP ? 1 : 0;
}

static struct grid_socktype grid_xreq_socktype_struct = {
    AF_SP_RAW,
    GRID_REQ,
    0,
    grid_xreq_create,
    grid_xreq_ispeer,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_socktype *grid_xreq_socktype = &grid_xreq_socktype_struct;


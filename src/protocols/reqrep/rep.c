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

#include "rep.h"
#include "xrep.h"

#include "../../grid.h"
#include "../../reqrep.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/chunkref.h"
#include "../../utils/wire.h"
#include "../../utils/list.h"
#include "../../utils/int.h"

#include <stddef.h>
#include <string.h>

#define GRID_REP_INPROGRESS 1

static const struct grid_sockbase_vfptr grid_rep_sockbase_vfptr = {
    NULL,
    grid_rep_destroy,
    grid_xrep_add,
    grid_xrep_rm,
    grid_xrep_in,
    grid_xrep_out,
    grid_rep_events,
    grid_rep_send,
    grid_rep_recv,
    grid_xrep_setopt,
    grid_xrep_getopt
};

void grid_rep_init (struct grid_rep *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint)
{
    grid_xrep_init (&self->xrep, vfptr, hint);
    self->flags = 0;
}

void grid_rep_term (struct grid_rep *self)
{
    if (self->flags & GRID_REP_INPROGRESS)
        grid_chunkref_term (&self->backtrace);
    grid_xrep_term (&self->xrep);
}

void grid_rep_destroy (struct grid_sockbase *self)
{
    struct grid_rep *rep;

    rep = grid_cont (self, struct grid_rep, xrep.sockbase);

    grid_rep_term (rep);
    grid_free (rep);
}

int grid_rep_events (struct grid_sockbase *self)
{
    struct grid_rep *rep;
    int events;

    rep = grid_cont (self, struct grid_rep, xrep.sockbase);
    events = grid_xrep_events (&rep->xrep.sockbase);
    if (!(rep->flags & GRID_REP_INPROGRESS))
        events &= ~GRID_SOCKBASE_EVENT_OUT;
    return events;
}

int grid_rep_send (struct grid_sockbase *self, struct grid_msg *msg)
{
    int rc;
    struct grid_rep *rep;

    rep = grid_cont (self, struct grid_rep, xrep.sockbase);

    /*  If no request was received, there's nowhere to send the reply to. */
    if (grid_slow (!(rep->flags & GRID_REP_INPROGRESS)))
        return -EFSM;

    /*  Move the stored backtrace into the message header. */
    grid_assert (grid_chunkref_size (&msg->sphdr) == 0);
    grid_chunkref_term (&msg->sphdr);
    grid_chunkref_mv (&msg->sphdr, &rep->backtrace);
    rep->flags &= ~GRID_REP_INPROGRESS;

    /*  Send the reply. If it cannot be sent because of pushback,
        drop it silently. */
    rc = grid_xrep_send (&rep->xrep.sockbase, msg);
    errnum_assert (rc == 0 || rc == -EAGAIN, -rc);

    return 0;
}

int grid_rep_recv (struct grid_sockbase *self, struct grid_msg *msg)
{
    int rc;
    struct grid_rep *rep;

    rep = grid_cont (self, struct grid_rep, xrep.sockbase);

    /*  If a request is already being processed, cancel it. */
    if (grid_slow (rep->flags & GRID_REP_INPROGRESS)) {
        grid_chunkref_term (&rep->backtrace);
        rep->flags &= ~GRID_REP_INPROGRESS;
    }

    /*  Receive the request. */
    rc = grid_xrep_recv (&rep->xrep.sockbase, msg);
    if (grid_slow (rc == -EAGAIN))
        return -EAGAIN;
    errnum_assert (rc == 0, -rc);

    /*  Store the backtrace. */
    grid_chunkref_mv (&rep->backtrace, &msg->sphdr);
    grid_chunkref_init (&msg->sphdr, 0);
    rep->flags |= GRID_REP_INPROGRESS;

    return 0;
}

static int grid_rep_create (void *hint, struct grid_sockbase **sockbase)
{
    struct grid_rep *self;

    self = grid_alloc (sizeof (struct grid_rep), "socket (rep)");
    alloc_assert (self);
    grid_rep_init (self, &grid_rep_sockbase_vfptr, hint);
    *sockbase = &self->xrep.sockbase;

    return 0;
}

static struct grid_socktype grid_rep_socktype_struct = {
    AF_SP,
    GRID_REP,
    0,
    grid_rep_create,
    grid_xrep_ispeer,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_socktype *grid_rep_socktype = &grid_rep_socktype_struct;


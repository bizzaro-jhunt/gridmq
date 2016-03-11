/*
    Copyright (c) 2012-2013 Martin Sustrik  All rights reserved.
    Copyright 2015 Garrett D'Amore <garrett@damore.org>

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

#include "respondent.h"
#include "xrespondent.h"

#include "../../grid.h"
#include "../../survey.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/alloc.h"
#include "../../utils/wire.h"
#include "../../utils/list.h"
#include "../../utils/int.h"

#include <string.h>

#define GRID_RESPONDENT_INPROGRESS 1

struct grid_respondent {
    struct grid_xrespondent xrespondent;
    uint32_t flags;
    struct grid_chunkref backtrace;
};

/*  Private functions. */
static void grid_respondent_init (struct grid_respondent *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint);
static void grid_respondent_term (struct grid_respondent *self);

/*  Implementation of grid_sockbase's virtual functions. */
static void grid_respondent_destroy (struct grid_sockbase *self);
static int grid_respondent_events (struct grid_sockbase *self);
static int grid_respondent_send (struct grid_sockbase *self, struct grid_msg *msg);
static int grid_respondent_recv (struct grid_sockbase *self, struct grid_msg *msg);
static const struct grid_sockbase_vfptr grid_respondent_sockbase_vfptr = {
    NULL,
    grid_respondent_destroy,
    grid_xrespondent_add,
    grid_xrespondent_rm,
    grid_xrespondent_in,
    grid_xrespondent_out,
    grid_respondent_events,
    grid_respondent_send,
    grid_respondent_recv,
    grid_xrespondent_setopt,
    grid_xrespondent_getopt
};

static void grid_respondent_init (struct grid_respondent *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint)
{
    grid_xrespondent_init (&self->xrespondent, vfptr, hint);
    self->flags = 0;
}

static void grid_respondent_term (struct grid_respondent *self)
{
    if (self->flags & GRID_RESPONDENT_INPROGRESS)
        grid_chunkref_term (&self->backtrace);
    grid_xrespondent_term (&self->xrespondent);
}

void grid_respondent_destroy (struct grid_sockbase *self)
{
    struct grid_respondent *respondent;

    respondent = grid_cont (self, struct grid_respondent, xrespondent.sockbase);

    grid_respondent_term (respondent);
    grid_free (respondent);
}

static int grid_respondent_events (struct grid_sockbase *self)
{
    int events;
    struct grid_respondent *respondent;

    respondent = grid_cont (self, struct grid_respondent, xrespondent.sockbase);

    events = grid_xrespondent_events (&respondent->xrespondent.sockbase);
    if (!(respondent->flags & GRID_RESPONDENT_INPROGRESS))
        events &= ~GRID_SOCKBASE_EVENT_OUT;
    return events;
}

static int grid_respondent_send (struct grid_sockbase *self, struct grid_msg *msg)
{
    int rc;
    struct grid_respondent *respondent;

    respondent = grid_cont (self, struct grid_respondent, xrespondent.sockbase);

    /*  If there's no survey going on, report EFSM error. */
    if (grid_slow (!(respondent->flags & GRID_RESPONDENT_INPROGRESS)))
        return -EFSM;

    /*  Tag the message with survey ID. */
    grid_assert (grid_chunkref_size (&msg->sphdr) == 0);
    grid_chunkref_term (&msg->sphdr);
    grid_chunkref_mv (&msg->sphdr, &respondent->backtrace);

    /*  Remember that no survey is being processed. */
    respondent->flags &= ~GRID_RESPONDENT_INPROGRESS;

    /*  Try to send the message. If it cannot be sent due to pushback, drop it
        silently. */
    rc = grid_xrespondent_send (&respondent->xrespondent.sockbase, msg);
    errnum_assert (rc == 0 || rc == -EAGAIN, -rc);

    return 0;
}

static int grid_respondent_recv (struct grid_sockbase *self, struct grid_msg *msg)
{
    int rc;
    struct grid_respondent *respondent;

    respondent = grid_cont (self, struct grid_respondent, xrespondent.sockbase);

    /*  Cancel current survey and clean up backtrace, if it exists. */
    if (grid_slow (respondent->flags & GRID_RESPONDENT_INPROGRESS)) {
        grid_chunkref_term (&respondent->backtrace);
        respondent->flags &= ~GRID_RESPONDENT_INPROGRESS;
    }

    /*  Get next survey. */
    rc = grid_xrespondent_recv (&respondent->xrespondent.sockbase, msg);
    if (grid_slow (rc == -EAGAIN))
        return -EAGAIN;
    errnum_assert (rc == 0, -rc);

    /*  Store the backtrace. */
    grid_chunkref_mv (&respondent->backtrace, &msg->sphdr);
    grid_chunkref_init (&msg->sphdr, 0);

    /*  Remember that survey is being processed. */
    respondent->flags |= GRID_RESPONDENT_INPROGRESS;

    return 0;
}

static int grid_respondent_create (void *hint, struct grid_sockbase **sockbase)
{
    struct grid_respondent *self;

    self = grid_alloc (sizeof (struct grid_respondent), "socket (respondent)");
    alloc_assert (self);
    grid_respondent_init (self, &grid_respondent_sockbase_vfptr, hint);
    *sockbase = &self->xrespondent.sockbase;

    return 0;
}

static struct grid_socktype grid_respondent_socktype_struct = {
    AF_SP,
    GRID_RESPONDENT,
    0,
    grid_respondent_create,
    grid_xrespondent_ispeer,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_socktype *grid_respondent_socktype = &grid_respondent_socktype_struct;


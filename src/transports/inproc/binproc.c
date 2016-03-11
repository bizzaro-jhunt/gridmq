/*
    Copyright (c) 2012-2013 Martin Sustrik  All rights reserved.
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

#include "binproc.h"
#include "sinproc.h"
#include "cinproc.h"
#include "ins.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/alloc.h"

#define GRID_BINPROC_STATE_IDLE 1
#define GRID_BINPROC_STATE_ACTIVE 2
#define GRID_BINPROC_STATE_STOPPING 3

#define GRID_BINPROC_SRC_SINPROC 1

/*  Implementation of grid_epbase interface. */
static void grid_binproc_stop (struct grid_epbase *self);
static void grid_binproc_destroy (struct grid_epbase *self);
static const struct grid_epbase_vfptr grid_binproc_vfptr = {
    grid_binproc_stop,
    grid_binproc_destroy
};

/*  Private functions. */
static void grid_binproc_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_binproc_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_binproc_connect (struct grid_ins_item *self,
    struct grid_ins_item *peer);


int grid_binproc_create (void *hint, struct grid_epbase **epbase)
{
    int rc;
    struct grid_binproc *self;

    self = grid_alloc (sizeof (struct grid_binproc), "binproc");
    alloc_assert (self);

    grid_ins_item_init (&self->item, &grid_binproc_vfptr, hint);
    grid_fsm_init_root (&self->fsm, grid_binproc_handler, grid_binproc_shutdown,
        grid_epbase_getctx (&self->item.epbase));
    self->state = GRID_BINPROC_STATE_IDLE;
    grid_list_init (&self->sinprocs);

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);

    /*  Register the inproc endpoint into a global repository. */
    rc = grid_ins_bind (&self->item, grid_binproc_connect);
    if (grid_slow (rc < 0)) {
        grid_list_term (&self->sinprocs);

        /*  TODO: Now, this is ugly! We are getting the state machine into
            the idle state manually. How should it be done correctly? */
        self->fsm.state = 1;
        grid_fsm_term (&self->fsm);

        grid_ins_item_term (&self->item);
        grid_free (self);
        return rc;
    }

    *epbase = &self->item.epbase;
    return 0;
}

static void grid_binproc_stop (struct grid_epbase *self)
{
    struct grid_binproc *binproc;

    binproc = grid_cont (self, struct grid_binproc, item.epbase);

    grid_fsm_stop (&binproc->fsm);
}

static void grid_binproc_destroy (struct grid_epbase *self)
{
    struct grid_binproc *binproc;

    binproc = grid_cont (self, struct grid_binproc, item.epbase);

    grid_list_term (&binproc->sinprocs);
    grid_fsm_term (&binproc->fsm);
    grid_ins_item_term (&binproc->item);

    grid_free (binproc);
}

static void grid_binproc_connect (struct grid_ins_item *self,
    struct grid_ins_item *peer)
{
    struct grid_binproc *binproc;
    struct grid_cinproc *cinproc;
    struct grid_sinproc *sinproc;

    binproc = grid_cont (self, struct grid_binproc, item);
    cinproc = grid_cont (peer, struct grid_cinproc, item);

    grid_assert_state (binproc, GRID_BINPROC_STATE_ACTIVE);

    sinproc = grid_alloc (sizeof (struct grid_sinproc), "sinproc");
    alloc_assert (sinproc);
    grid_sinproc_init (sinproc, GRID_BINPROC_SRC_SINPROC,
        &binproc->item.epbase, &binproc->fsm);
    grid_list_insert (&binproc->sinprocs, &sinproc->item,
        grid_list_end (&binproc->sinprocs));
    grid_sinproc_connect (sinproc, &cinproc->fsm);

    grid_epbase_stat_increment (&binproc->item.epbase,
        GRID_STAT_ACCEPTED_CONNECTIONS, 1);
}

static void grid_binproc_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    struct grid_binproc *binproc;
    struct grid_list_item *it;
    struct grid_sinproc *sinproc;

    binproc = grid_cont (self, struct grid_binproc, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {

        /*  First, unregister the endpoint from the global repository of inproc
            endpoints. This way, new connections cannot be created anymore. */
        grid_ins_unbind (&binproc->item);

        /*  Stop the existing connections. */
        for (it = grid_list_begin (&binproc->sinprocs);
              it != grid_list_end (&binproc->sinprocs);
              it = grid_list_next (&binproc->sinprocs, it)) {
            sinproc = grid_cont (it, struct grid_sinproc, item);
            grid_sinproc_stop (sinproc);
        }

        binproc->state = GRID_BINPROC_STATE_STOPPING;
        goto finish;
    }
    if (grid_slow (binproc->state == GRID_BINPROC_STATE_STOPPING)) {
        grid_assert (src == GRID_BINPROC_SRC_SINPROC && type == GRID_SINPROC_STOPPED);
        sinproc = (struct grid_sinproc*) srcptr;
        grid_list_erase (&binproc->sinprocs, &sinproc->item);
        grid_sinproc_term (sinproc);
        grid_free (sinproc);
finish:
        if (!grid_list_empty (&binproc->sinprocs))
            return;
        binproc->state = GRID_BINPROC_STATE_IDLE;
        grid_fsm_stopped_noevent (&binproc->fsm);
        grid_epbase_stopped (&binproc->item.epbase);
        return;
    }

    grid_fsm_bad_state(binproc->state, src, type);
}

static void grid_binproc_handler (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    struct grid_binproc *binproc;
    struct grid_sinproc *peer;
    struct grid_sinproc *sinproc;

    binproc = grid_cont (self, struct grid_binproc, fsm);

    switch (binproc->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case GRID_BINPROC_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                binproc->state = GRID_BINPROC_STATE_ACTIVE;
                return;
            default:
                grid_fsm_bad_action (binproc->state, src, type);
            }

        default:
            grid_fsm_bad_source (binproc->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case GRID_BINPROC_STATE_ACTIVE:
        switch (src) {

        case GRID_SINPROC_SRC_PEER:
            switch (type) {
            case GRID_SINPROC_CONNECT:
                peer = (struct grid_sinproc*) srcptr;
                sinproc = grid_alloc (sizeof (struct grid_sinproc), "sinproc");
                alloc_assert (sinproc);
                grid_sinproc_init (sinproc, GRID_BINPROC_SRC_SINPROC,
                    &binproc->item.epbase, &binproc->fsm);
                grid_list_insert (&binproc->sinprocs, &sinproc->item,
                    grid_list_end (&binproc->sinprocs));
                grid_sinproc_accept (sinproc, peer);
                return;
            default:
                grid_fsm_bad_action (binproc->state, src, type);
            }

        case GRID_BINPROC_SRC_SINPROC:
            return;

        default:
            grid_fsm_bad_source (binproc->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (binproc->state, src, type);
    }
}


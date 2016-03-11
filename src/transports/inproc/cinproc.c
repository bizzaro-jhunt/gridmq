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

#include "cinproc.h"
#include "binproc.h"
#include "ins.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/attr.h"

#include <stddef.h>

#define GRID_CINPROC_STATE_IDLE 1
#define GRID_CINPROC_STATE_DISCONNECTED 2
#define GRID_CINPROC_STATE_ACTIVE 3
#define GRID_CINPROC_STATE_STOPPING 4

#define GRID_CINPROC_ACTION_CONNECT 1

#define GRID_CINPROC_SRC_SINPROC 1

/*  Implementation of grid_epbase callback interface. */
static void grid_cinproc_stop (struct grid_epbase *self);
static void grid_cinproc_destroy (struct grid_epbase *self);
static const struct grid_epbase_vfptr grid_cinproc_vfptr = {
    grid_cinproc_stop,
    grid_cinproc_destroy
};

/*  Private functions. */
static void grid_cinproc_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_cinproc_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_cinproc_connect (struct grid_ins_item *self,
    struct grid_ins_item *peer);

int grid_cinproc_create (void *hint, struct grid_epbase **epbase)
{
    struct grid_cinproc *self;

    self = grid_alloc (sizeof (struct grid_cinproc), "cinproc");
    alloc_assert (self);

    grid_ins_item_init (&self->item, &grid_cinproc_vfptr, hint);
    grid_fsm_init_root (&self->fsm, grid_cinproc_handler, grid_cinproc_shutdown,
        grid_epbase_getctx (&self->item.epbase));
    self->state = GRID_CINPROC_STATE_IDLE;
    grid_sinproc_init (&self->sinproc, GRID_CINPROC_SRC_SINPROC,
        &self->item.epbase, &self->fsm);

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);

    /*  Register the inproc endpoint into a global repository. */
    grid_ins_connect (&self->item, grid_cinproc_connect);

    *epbase = &self->item.epbase;
    return 0;
}

static void grid_cinproc_stop (struct grid_epbase *self)
{
    struct grid_cinproc *cinproc;

    cinproc = grid_cont (self, struct grid_cinproc, item.epbase);

    grid_fsm_stop (&cinproc->fsm);
}

static void grid_cinproc_destroy (struct grid_epbase *self)
{
    struct grid_cinproc *cinproc;

    cinproc = grid_cont (self, struct grid_cinproc, item.epbase);

    grid_sinproc_term (&cinproc->sinproc);
    grid_fsm_term (&cinproc->fsm);
    grid_ins_item_term (&cinproc->item);

    grid_free (cinproc);
}

static void grid_cinproc_connect (struct grid_ins_item *self,
    struct grid_ins_item *peer)
{
    struct grid_cinproc *cinproc;
    struct grid_binproc *binproc;

    cinproc = grid_cont (self, struct grid_cinproc, item);
    binproc = grid_cont (peer, struct grid_binproc, item);

    grid_assert_state (cinproc, GRID_CINPROC_STATE_DISCONNECTED);
    grid_sinproc_connect (&cinproc->sinproc, &binproc->fsm);
    grid_fsm_action (&cinproc->fsm, GRID_CINPROC_ACTION_CONNECT);
}

static void grid_cinproc_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_cinproc *cinproc;

    cinproc = grid_cont (self, struct grid_cinproc, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {

        /*  First, unregister the endpoint from the global repository of inproc
            endpoints. This way, new connections cannot be created anymore. */
        grid_ins_disconnect (&cinproc->item);

        /*  Stop the existing connection. */
        grid_sinproc_stop (&cinproc->sinproc);
        cinproc->state = GRID_CINPROC_STATE_STOPPING;
    }
    if (grid_slow (cinproc->state == GRID_CINPROC_STATE_STOPPING)) {
        if (!grid_sinproc_isidle (&cinproc->sinproc))
            return;
        cinproc->state = GRID_CINPROC_STATE_IDLE;
        grid_fsm_stopped_noevent (&cinproc->fsm);
        grid_epbase_stopped (&cinproc->item.epbase);
        return;
    }

    grid_fsm_bad_state(cinproc->state, src, type);
}

static void grid_cinproc_handler (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    struct grid_cinproc *cinproc;
    struct grid_sinproc *sinproc;

    cinproc = grid_cont (self, struct grid_cinproc, fsm);


    switch (cinproc->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case GRID_CINPROC_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                cinproc->state = GRID_CINPROC_STATE_DISCONNECTED;
                grid_epbase_stat_increment (&cinproc->item.epbase,
                    GRID_STAT_INPROGRESS_CONNECTIONS, 1);
                return;
            default:
                grid_fsm_bad_action (cinproc->state, src, type);
            }

        default:
            grid_fsm_bad_source (cinproc->state, src, type);
        }

/******************************************************************************/
/*  DISCONNECTED state.                                                       */
/******************************************************************************/
    case GRID_CINPROC_STATE_DISCONNECTED:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_CINPROC_ACTION_CONNECT:
                cinproc->state = GRID_CINPROC_STATE_ACTIVE;
                grid_epbase_stat_increment (&cinproc->item.epbase,
                    GRID_STAT_INPROGRESS_CONNECTIONS, -1);
                grid_epbase_stat_increment (&cinproc->item.epbase,
                    GRID_STAT_ESTABLISHED_CONNECTIONS, 1);
                return;
            default:
                grid_fsm_bad_action (cinproc->state, src, type);
            }

        case GRID_SINPROC_SRC_PEER:
            sinproc = (struct grid_sinproc*) srcptr;
            switch (type) {
            case GRID_SINPROC_CONNECT:
                grid_sinproc_accept (&cinproc->sinproc, sinproc);
                cinproc->state = GRID_CINPROC_STATE_ACTIVE;
                grid_epbase_stat_increment (&cinproc->item.epbase,
                    GRID_STAT_INPROGRESS_CONNECTIONS, -1);
                grid_epbase_stat_increment (&cinproc->item.epbase,
                    GRID_STAT_ESTABLISHED_CONNECTIONS, 1);
                return;
            default:
                grid_fsm_bad_action (cinproc->state, src, type);
            }

        default:
            grid_fsm_bad_source (cinproc->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case GRID_CINPROC_STATE_ACTIVE:
        switch (src) {
        case GRID_CINPROC_SRC_SINPROC:
            switch (type) {
            case GRID_SINPROC_DISCONNECT:
                cinproc->state = GRID_CINPROC_STATE_DISCONNECTED;
                grid_epbase_stat_increment (&cinproc->item.epbase,
                    GRID_STAT_INPROGRESS_CONNECTIONS, 1);

                grid_sinproc_init (&cinproc->sinproc, GRID_CINPROC_SRC_SINPROC,
                    &cinproc->item.epbase, &cinproc->fsm);
                return;

            default:
                grid_fsm_bad_action (cinproc->state, src, type);
            }

        default:
            grid_fsm_bad_source (cinproc->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (cinproc->state, src, type);
    }
}


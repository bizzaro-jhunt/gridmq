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

#include "surveyor.h"
#include "xsurveyor.h"

#include "../../grid.h"
#include "../../survey.h"

#include "../../aio/fsm.h"
#include "../../aio/timer.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/wire.h"
#include "../../utils/alloc.h"
#include "../../utils/random.h"
#include "../../utils/list.h"
#include "../../utils/int.h"
#include "../../utils/attr.h"

#include <string.h>

#define GRID_SURVEYOR_DEFAULT_DEADLINE 1000

#define GRID_SURVEYOR_STATE_IDLE 1
#define GRID_SURVEYOR_STATE_PASSIVE 2
#define GRID_SURVEYOR_STATE_ACTIVE 3
#define GRID_SURVEYOR_STATE_CANCELLING 4
#define GRID_SURVEYOR_STATE_STOPPING_TIMER 5
#define GRID_SURVEYOR_STATE_STOPPING 6

#define GRID_SURVEYOR_ACTION_START 1
#define GRID_SURVEYOR_ACTION_CANCEL 2

#define GRID_SURVEYOR_SRC_DEADLINE_TIMER 1

#define GRID_SURVEYOR_TIMEDOUT 1

struct grid_surveyor {

    /*  The underlying raw SP socket. */
    struct grid_xsurveyor xsurveyor;

    /*  The state machine. */
    struct grid_fsm fsm;
    int state;

    /*  Survey ID of the current survey. */
    uint32_t surveyid;

    /*  Timer for timing out the survey. */
    struct grid_timer timer;

    /*  When starting the survey, the message is temporarily stored here. */
    struct grid_msg tosend;

    /*  Protocol-specific socket options. */
    int deadline;

    /*  Flag if surveyor has timed out */
    int timedout;
};

/*  Private functions. */
static void grid_surveyor_init (struct grid_surveyor *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint);
static void grid_surveyor_term (struct grid_surveyor *self);
static void grid_surveyor_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_surveyor_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);
static int grid_surveyor_inprogress (struct grid_surveyor *self);
static void grid_surveyor_resend (struct grid_surveyor *self);

/*  Implementation of grid_sockbase's virtual functions. */
static void grid_surveyor_stop (struct grid_sockbase *self);
static void grid_surveyor_destroy (struct grid_sockbase *self);
static int grid_surveyor_events (struct grid_sockbase *self);
static int grid_surveyor_send (struct grid_sockbase *self, struct grid_msg *msg);
static int grid_surveyor_recv (struct grid_sockbase *self, struct grid_msg *msg);
static int grid_surveyor_setopt (struct grid_sockbase *self, int level, int option,
    const void *optval, size_t optvallen);
static int grid_surveyor_getopt (struct grid_sockbase *self, int level, int option,
    void *optval, size_t *optvallen);
static const struct grid_sockbase_vfptr grid_surveyor_sockbase_vfptr = {
    grid_surveyor_stop,
    grid_surveyor_destroy,
    grid_xsurveyor_add,
    grid_xsurveyor_rm,
    grid_xsurveyor_in,
    grid_xsurveyor_out,
    grid_surveyor_events,
    grid_surveyor_send,
    grid_surveyor_recv,
    grid_surveyor_setopt,
    grid_surveyor_getopt
};

static void grid_surveyor_init (struct grid_surveyor *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint)
{
    grid_xsurveyor_init (&self->xsurveyor, vfptr, hint);
    grid_fsm_init_root (&self->fsm, grid_surveyor_handler, grid_surveyor_shutdown,
        grid_sockbase_getctx (&self->xsurveyor.sockbase));
    self->state = GRID_SURVEYOR_STATE_IDLE;

    /*  Start assigning survey IDs beginning with a random number. This way
        there should be no key clashes even if the executable is re-started. */
    grid_random_generate (&self->surveyid, sizeof (self->surveyid));

    grid_timer_init (&self->timer, GRID_SURVEYOR_SRC_DEADLINE_TIMER, &self->fsm);
    grid_msg_init (&self->tosend, 0);
    self->deadline = GRID_SURVEYOR_DEFAULT_DEADLINE;
    self->timedout = 0;

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);
}

static void grid_surveyor_term (struct grid_surveyor *self)
{
    grid_msg_term (&self->tosend);
    grid_timer_term (&self->timer);
    grid_fsm_term (&self->fsm);
    grid_xsurveyor_term (&self->xsurveyor);
}

void grid_surveyor_stop (struct grid_sockbase *self)
{
    struct grid_surveyor *surveyor;

    surveyor = grid_cont (self, struct grid_surveyor, xsurveyor.sockbase);

    grid_fsm_stop (&surveyor->fsm);
}

void grid_surveyor_destroy (struct grid_sockbase *self)
{
    struct grid_surveyor *surveyor;

    surveyor = grid_cont (self, struct grid_surveyor, xsurveyor.sockbase);

    grid_surveyor_term (surveyor);
    grid_free (surveyor);
}

static int grid_surveyor_inprogress (struct grid_surveyor *self)
{
    /*  Return 1 if there's a survey going on. 0 otherwise. */
    return self->state == GRID_SURVEYOR_STATE_IDLE ||
        self->state == GRID_SURVEYOR_STATE_PASSIVE ||
        self->state == GRID_SURVEYOR_STATE_STOPPING ? 0 : 1;
}

static int grid_surveyor_events (struct grid_sockbase *self)
{
    int rc;
    struct grid_surveyor *surveyor;

    surveyor = grid_cont (self, struct grid_surveyor, xsurveyor.sockbase);

    /*  Determine the actual readability/writability of the socket. */
    rc = grid_xsurveyor_events (&surveyor->xsurveyor.sockbase);

    /*  If there's no survey going on we'll signal IN to interrupt polling
        when the survey expires. grid_recv() will return -EFSM afterwards. */
    if (!grid_surveyor_inprogress (surveyor))
        rc |= GRID_SOCKBASE_EVENT_IN;

    return rc;
}

static int grid_surveyor_send (struct grid_sockbase *self, struct grid_msg *msg)
{
    struct grid_surveyor *surveyor;

    surveyor = grid_cont (self, struct grid_surveyor, xsurveyor.sockbase);

    /*  Generate new survey ID. */
    ++surveyor->surveyid;
    surveyor->surveyid |= 0x80000000;

    /*  Tag the survey body with survey ID. */
    grid_assert (grid_chunkref_size (&msg->sphdr) == 0);
    grid_chunkref_term (&msg->sphdr);
    grid_chunkref_init (&msg->sphdr, 4);
    grid_putl (grid_chunkref_data (&msg->sphdr), surveyor->surveyid);

    /*  Store the survey, so that it can be sent later on. */
    grid_msg_term (&surveyor->tosend);
    grid_msg_mv (&surveyor->tosend, msg);
    grid_msg_init (msg, 0);

    /*  Cancel any ongoing survey, if any. */
    if (grid_slow (grid_surveyor_inprogress (surveyor))) {

        /*  First check whether the survey can be sent at all. */
        if (!(grid_xsurveyor_events (&surveyor->xsurveyor.sockbase) &
              GRID_SOCKBASE_EVENT_OUT))
            return -EAGAIN;

        /*  Cancel the current survey. */
        grid_fsm_action (&surveyor->fsm, GRID_SURVEYOR_ACTION_CANCEL);

        return 0;
    }

    /*  Notify the state machine that the survey was started. */
    grid_fsm_action (&surveyor->fsm, GRID_SURVEYOR_ACTION_START);

    return 0;
}

static int grid_surveyor_recv (struct grid_sockbase *self, struct grid_msg *msg)
{
    int rc;
    struct grid_surveyor *surveyor;
    uint32_t surveyid;

    surveyor = grid_cont (self, struct grid_surveyor, xsurveyor.sockbase);

    /*  If no survey is going on return EFSM error. */
    if (grid_slow (!grid_surveyor_inprogress (surveyor))) {
        if (surveyor->timedout == GRID_SURVEYOR_TIMEDOUT) {
            surveyor->timedout = 0;
            return -ETIMEDOUT;
        } else
            return -EFSM;
    }

    while (1) {

        /*  Get next response. */
        rc = grid_xsurveyor_recv (&surveyor->xsurveyor.sockbase, msg);
        if (grid_slow (rc == -EAGAIN))
            return -EAGAIN;
        errnum_assert (rc == 0, -rc);

        /*  Get the survey ID. Ignore any stale responses. */
        /*  TODO: This should be done asynchronously! */
        if (grid_slow (grid_chunkref_size (&msg->sphdr) != sizeof (uint32_t)))
            continue;
        surveyid = grid_getl (grid_chunkref_data (&msg->sphdr));
        if (grid_slow (surveyid != surveyor->surveyid))
            continue;

        /*  Discard the header and return the message to the user. */
        grid_chunkref_term (&msg->sphdr);
        grid_chunkref_init (&msg->sphdr, 0);
        break;
    }

    return 0;
}

static int grid_surveyor_setopt (struct grid_sockbase *self, int level, int option,
    const void *optval, size_t optvallen)
{
    struct grid_surveyor *surveyor;

    surveyor = grid_cont (self, struct grid_surveyor, xsurveyor.sockbase);

    if (level != GRID_SURVEYOR)
        return -ENOPROTOOPT;

    if (option == GRID_SURVEYOR_DEADLINE) {
        if (grid_slow (optvallen != sizeof (int)))
            return -EINVAL;
        surveyor->deadline = *(int*) optval;
        return 0;
    }

    return -ENOPROTOOPT;
}

static int grid_surveyor_getopt (struct grid_sockbase *self, int level, int option,
    void *optval, size_t *optvallen)
{
    struct grid_surveyor *surveyor;

    surveyor = grid_cont (self, struct grid_surveyor, xsurveyor.sockbase);

    if (level != GRID_SURVEYOR)
        return -ENOPROTOOPT;

    if (option == GRID_SURVEYOR_DEADLINE) {
        if (grid_slow (*optvallen < sizeof (int)))
            return -EINVAL;
        *(int*) optval = surveyor->deadline;
        *optvallen = sizeof (int);
        return 0;
    }

    return -ENOPROTOOPT;
}

static void grid_surveyor_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_surveyor *surveyor;

    surveyor = grid_cont (self, struct grid_surveyor, fsm);

    if (grid_slow (src== GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        grid_timer_stop (&surveyor->timer);
        surveyor->state = GRID_SURVEYOR_STATE_STOPPING;
    }
    if (grid_slow (surveyor->state == GRID_SURVEYOR_STATE_STOPPING)) {
        if (!grid_timer_isidle (&surveyor->timer))
            return;
        surveyor->state = GRID_SURVEYOR_STATE_IDLE;
        grid_fsm_stopped_noevent (&surveyor->fsm);
        grid_sockbase_stopped (&surveyor->xsurveyor.sockbase);
        return;
    }

    grid_fsm_bad_state(surveyor->state, src, type);
}

static void grid_surveyor_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_surveyor *surveyor;

    surveyor = grid_cont (self, struct grid_surveyor, fsm);

    switch (surveyor->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  The socket was created recently.                                          */
/******************************************************************************/
    case GRID_SURVEYOR_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                surveyor->state = GRID_SURVEYOR_STATE_PASSIVE;
                return;
            default:
                grid_fsm_bad_action (surveyor->state, src, type);
            }

        default:
            grid_fsm_bad_source (surveyor->state, src, type);
        }

/******************************************************************************/
/*  PASSIVE state.                                                            */
/*  There's no survey going on.                                               */
/******************************************************************************/
    case GRID_SURVEYOR_STATE_PASSIVE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_SURVEYOR_ACTION_START:
                grid_surveyor_resend (surveyor);
                grid_timer_start (&surveyor->timer, surveyor->deadline);
                surveyor->state = GRID_SURVEYOR_STATE_ACTIVE;
                return;

            default:
                grid_fsm_bad_action (surveyor->state, src, type);
            }

        default:
            grid_fsm_bad_source (surveyor->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  Survey was sent, waiting for responses.                                   */
/******************************************************************************/
    case GRID_SURVEYOR_STATE_ACTIVE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_SURVEYOR_ACTION_CANCEL:
                grid_timer_stop (&surveyor->timer);
                surveyor->state = GRID_SURVEYOR_STATE_CANCELLING;
                return;
            default:
                grid_fsm_bad_action (surveyor->state, src, type);
            }

        case GRID_SURVEYOR_SRC_DEADLINE_TIMER:
            switch (type) {
            case GRID_TIMER_TIMEOUT:
                grid_timer_stop (&surveyor->timer);
                surveyor->state = GRID_SURVEYOR_STATE_STOPPING_TIMER;
                surveyor->timedout = GRID_SURVEYOR_TIMEDOUT;
                return;
            default:
                grid_fsm_bad_action (surveyor->state, src, type);
            }

        default:
            grid_fsm_bad_source (surveyor->state, src, type);
        }

/******************************************************************************/
/*  CANCELLING state.                                                         */
/*  Survey was cancelled, but the old timer haven't stopped yet. The new      */
/*  survey thus haven't been sent and is stored in 'tosend'.                  */
/******************************************************************************/
    case GRID_SURVEYOR_STATE_CANCELLING:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_SURVEYOR_ACTION_CANCEL:
                return;
            default:
                grid_fsm_bad_action (surveyor->state, src, type);
            }

        case GRID_SURVEYOR_SRC_DEADLINE_TIMER:
            switch (type) {
            case GRID_TIMER_STOPPED:
                grid_surveyor_resend (surveyor);
                grid_timer_start (&surveyor->timer, surveyor->deadline);
                surveyor->state = GRID_SURVEYOR_STATE_ACTIVE;
                return;
            default:
                grid_fsm_bad_action (surveyor->state, src, type);
            }

        default:
            grid_fsm_bad_source (surveyor->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_TIMER state.                                                     */
/*  Survey timeout expired. Now we are stopping the timer.                    */
/******************************************************************************/
    case GRID_SURVEYOR_STATE_STOPPING_TIMER:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_SURVEYOR_ACTION_CANCEL:
                surveyor->state = GRID_SURVEYOR_STATE_CANCELLING;
                return;
            default:
                grid_fsm_bad_action (surveyor->state, src, type);
            }

        case GRID_SURVEYOR_SRC_DEADLINE_TIMER:
            switch (type) {
            case GRID_TIMER_STOPPED:
                surveyor->state = GRID_SURVEYOR_STATE_PASSIVE;
                return;
            default:
                grid_fsm_bad_action (surveyor->state, src, type);
            }

        default:
            grid_fsm_bad_source (surveyor->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (surveyor->state, src, type);
    }
}

static void grid_surveyor_resend (struct grid_surveyor *self)
{
    int rc;
    struct grid_msg msg;

    grid_msg_cp (&msg, &self->tosend);
    rc = grid_xsurveyor_send (&self->xsurveyor.sockbase, &msg);
    errnum_assert (rc == 0, -rc);
}

static int grid_surveyor_create (void *hint, struct grid_sockbase **sockbase)
{
    struct grid_surveyor *self;

    self = grid_alloc (sizeof (struct grid_surveyor), "socket (surveyor)");
    alloc_assert (self);
    grid_surveyor_init (self, &grid_surveyor_sockbase_vfptr, hint);
    *sockbase = &self->xsurveyor.sockbase;

    return 0;
}

static struct grid_socktype grid_surveyor_socktype_struct = {
    AF_SP,
    GRID_SURVEYOR,
    0,
    grid_surveyor_create,
    grid_xsurveyor_ispeer,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_socktype *grid_surveyor_socktype = &grid_surveyor_socktype_struct;


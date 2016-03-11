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

#include "atcp.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/attr.h"

#define GRID_ATCP_STATE_IDLE 1
#define GRID_ATCP_STATE_ACCEPTING 2
#define GRID_ATCP_STATE_ACTIVE 3
#define GRID_ATCP_STATE_STOPPING_STCP 4
#define GRID_ATCP_STATE_STOPPING_USOCK 5
#define GRID_ATCP_STATE_DONE 6
#define GRID_ATCP_STATE_STOPPING_STCP_FINAL 7
#define GRID_ATCP_STATE_STOPPING 8

#define GRID_ATCP_SRC_USOCK 1
#define GRID_ATCP_SRC_STCP 2
#define GRID_ATCP_SRC_LISTENER 3

/*  Private functions. */
static void grid_atcp_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_atcp_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);

void grid_atcp_init (struct grid_atcp *self, int src,
    struct grid_epbase *epbase, struct grid_fsm *owner)
{
    grid_fsm_init (&self->fsm, grid_atcp_handler, grid_atcp_shutdown,
        src, self, owner);
    self->state = GRID_ATCP_STATE_IDLE;
    self->epbase = epbase;
    grid_usock_init (&self->usock, GRID_ATCP_SRC_USOCK, &self->fsm);
    self->listener = NULL;
    self->listener_owner.src = -1;
    self->listener_owner.fsm = NULL;
    grid_stcp_init (&self->stcp, GRID_ATCP_SRC_STCP, epbase, &self->fsm);
    grid_fsm_event_init (&self->accepted);
    grid_fsm_event_init (&self->done);
    grid_list_item_init (&self->item);
}

void grid_atcp_term (struct grid_atcp *self)
{
    grid_assert_state (self, GRID_ATCP_STATE_IDLE);

    grid_list_item_term (&self->item);
    grid_fsm_event_term (&self->done);
    grid_fsm_event_term (&self->accepted);
    grid_stcp_term (&self->stcp);
    grid_usock_term (&self->usock);
    grid_fsm_term (&self->fsm);
}

int grid_atcp_isidle (struct grid_atcp *self)
{
    return grid_fsm_isidle (&self->fsm);
}

void grid_atcp_start (struct grid_atcp *self, struct grid_usock *listener)
{
    grid_assert_state (self, GRID_ATCP_STATE_IDLE);

    /*  Take ownership of the listener socket. */
    self->listener = listener;
    self->listener_owner.src = GRID_ATCP_SRC_LISTENER;
    self->listener_owner.fsm = &self->fsm;
    grid_usock_swap_owner (listener, &self->listener_owner);

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);
}

void grid_atcp_stop (struct grid_atcp *self)
{
    grid_fsm_stop (&self->fsm);
}

static void grid_atcp_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_atcp *atcp;

    atcp = grid_cont (self, struct grid_atcp, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        if (!grid_stcp_isidle (&atcp->stcp)) {
            grid_epbase_stat_increment (atcp->epbase,
                GRID_STAT_DROPPED_CONNECTIONS, 1);
            grid_stcp_stop (&atcp->stcp);
        }
        atcp->state = GRID_ATCP_STATE_STOPPING_STCP_FINAL;
    }
    if (grid_slow (atcp->state == GRID_ATCP_STATE_STOPPING_STCP_FINAL)) {
        if (!grid_stcp_isidle (&atcp->stcp))
            return;
        grid_usock_stop (&atcp->usock);
        atcp->state = GRID_ATCP_STATE_STOPPING;
    }
    if (grid_slow (atcp->state == GRID_ATCP_STATE_STOPPING)) {
        if (!grid_usock_isidle (&atcp->usock))
            return;
       if (atcp->listener) {
            grid_assert (atcp->listener_owner.fsm);
            grid_usock_swap_owner (atcp->listener, &atcp->listener_owner);
            atcp->listener = NULL;
            atcp->listener_owner.src = -1;
            atcp->listener_owner.fsm = NULL;
        }
        atcp->state = GRID_ATCP_STATE_IDLE;
        grid_fsm_stopped (&atcp->fsm, GRID_ATCP_STOPPED);
        return;
    }

    grid_fsm_bad_action(atcp->state, src, type);
}

static void grid_atcp_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_atcp *atcp;
    int val;
    size_t sz;

    atcp = grid_cont (self, struct grid_atcp, fsm);

    switch (atcp->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  The state machine wasn't yet started.                                     */
/******************************************************************************/
    case GRID_ATCP_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_usock_accept (&atcp->usock, atcp->listener);
                atcp->state = GRID_ATCP_STATE_ACCEPTING;
                return;
            default:
                grid_fsm_bad_action (atcp->state, src, type);
            }

        default:
            grid_fsm_bad_source (atcp->state, src, type);
        }

/******************************************************************************/
/*  ACCEPTING state.                                                          */
/*  Waiting for incoming connection.                                          */
/******************************************************************************/
    case GRID_ATCP_STATE_ACCEPTING:
        switch (src) {

        case GRID_ATCP_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_ACCEPTED:
                grid_epbase_clear_error (atcp->epbase);

                /*  Set the relevant socket options. */
                sz = sizeof (val);
                grid_epbase_getopt (atcp->epbase, GRID_SOL_SOCKET, GRID_SNDBUF,
                    &val, &sz);
                grid_assert (sz == sizeof (val));
                grid_usock_setsockopt (&atcp->usock, SOL_SOCKET, SO_SNDBUF,
                    &val, sizeof (val));
                sz = sizeof (val);
                grid_epbase_getopt (atcp->epbase, GRID_SOL_SOCKET, GRID_RCVBUF,
                    &val, &sz);
                grid_assert (sz == sizeof (val));
                grid_usock_setsockopt (&atcp->usock, SOL_SOCKET, SO_RCVBUF,
                    &val, sizeof (val));

                /*  Return ownership of the listening socket to the parent. */
                grid_usock_swap_owner (atcp->listener, &atcp->listener_owner);
                atcp->listener = NULL;
                atcp->listener_owner.src = -1;
                atcp->listener_owner.fsm = NULL;
                grid_fsm_raise (&atcp->fsm, &atcp->accepted, GRID_ATCP_ACCEPTED);

                /*  Start the stcp state machine. */
                grid_usock_activate (&atcp->usock);
                grid_stcp_start (&atcp->stcp, &atcp->usock);
                atcp->state = GRID_ATCP_STATE_ACTIVE;

                grid_epbase_stat_increment (atcp->epbase,
                    GRID_STAT_ACCEPTED_CONNECTIONS, 1);

                return;

            default:
                grid_fsm_bad_action (atcp->state, src, type);
            }

        case GRID_ATCP_SRC_LISTENER:
            switch (type) {

            case GRID_USOCK_ACCEPT_ERROR:
                grid_epbase_set_error (atcp->epbase,
                    grid_usock_geterrno(atcp->listener));
                grid_epbase_stat_increment (atcp->epbase,
                    GRID_STAT_ACCEPT_ERRORS, 1);
                grid_usock_accept (&atcp->usock, atcp->listener);
                return;

            default:
                grid_fsm_bad_action (atcp->state, src, type);
            }

        default:
            grid_fsm_bad_source (atcp->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case GRID_ATCP_STATE_ACTIVE:
        switch (src) {

        case GRID_ATCP_SRC_STCP:
            switch (type) {
            case GRID_STCP_ERROR:
                grid_stcp_stop (&atcp->stcp);
                atcp->state = GRID_ATCP_STATE_STOPPING_STCP;
                grid_epbase_stat_increment (atcp->epbase,
                    GRID_STAT_BROKEN_CONNECTIONS, 1);
                return;
            default:
                grid_fsm_bad_action (atcp->state, src, type);
            }

        default:
            grid_fsm_bad_source (atcp->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_STCP state.                                                      */
/******************************************************************************/
    case GRID_ATCP_STATE_STOPPING_STCP:
        switch (src) {

        case GRID_ATCP_SRC_STCP:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_STCP_STOPPED:
                grid_usock_stop (&atcp->usock);
                atcp->state = GRID_ATCP_STATE_STOPPING_USOCK;
                return;
            default:
                grid_fsm_bad_action (atcp->state, src, type);
            }

        default:
            grid_fsm_bad_source (atcp->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_USOCK state.                                                      */
/******************************************************************************/
    case GRID_ATCP_STATE_STOPPING_USOCK:
        switch (src) {

        case GRID_ATCP_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_USOCK_STOPPED:
                grid_fsm_raise (&atcp->fsm, &atcp->done, GRID_ATCP_ERROR);
                atcp->state = GRID_ATCP_STATE_DONE;
                return;
            default:
                grid_fsm_bad_action (atcp->state, src, type);
            }

        default:
            grid_fsm_bad_source (atcp->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (atcp->state, src, type);
    }
}


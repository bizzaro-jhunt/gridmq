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

#include "aipc.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/attr.h"

#define GRID_AIPC_STATE_IDLE 1
#define GRID_AIPC_STATE_ACCEPTING 2
#define GRID_AIPC_STATE_ACTIVE 3
#define GRID_AIPC_STATE_STOPPING_SIPC 4
#define GRID_AIPC_STATE_STOPPING_USOCK 5
#define GRID_AIPC_STATE_DONE 6
#define GRID_AIPC_STATE_STOPPING_SIPC_FINAL 7
#define GRID_AIPC_STATE_STOPPING 8

#define GRID_AIPC_SRC_USOCK 1
#define GRID_AIPC_SRC_SIPC 2
#define GRID_AIPC_SRC_LISTENER 3

/*  Private functions. */
static void grid_aipc_handler (struct grid_fsm *self, int src, int type,
   void *srcptr);
static void grid_aipc_shutdown (struct grid_fsm *self, int src, int type,
   void *srcptr);

void grid_aipc_init (struct grid_aipc *self, int src,
    struct grid_epbase *epbase, struct grid_fsm *owner)
{
    grid_fsm_init (&self->fsm, grid_aipc_handler, grid_aipc_shutdown,
        src, self, owner);
    self->state = GRID_AIPC_STATE_IDLE;
    self->epbase = epbase;
    grid_usock_init (&self->usock, GRID_AIPC_SRC_USOCK, &self->fsm);
    self->listener = NULL;
    self->listener_owner.src = -1;
    self->listener_owner.fsm = NULL;
    grid_sipc_init (&self->sipc, GRID_AIPC_SRC_SIPC, epbase, &self->fsm);
    grid_fsm_event_init (&self->accepted);
    grid_fsm_event_init (&self->done);
    grid_list_item_init (&self->item);
}

void grid_aipc_term (struct grid_aipc *self)
{
    grid_assert_state (self, GRID_AIPC_STATE_IDLE);

    grid_list_item_term (&self->item);
    grid_fsm_event_term (&self->done);
    grid_fsm_event_term (&self->accepted);
    grid_sipc_term (&self->sipc);
    grid_usock_term (&self->usock);
    grid_fsm_term (&self->fsm);
}

int grid_aipc_isidle (struct grid_aipc *self)
{
    return grid_fsm_isidle (&self->fsm);
}

void grid_aipc_start (struct grid_aipc *self, struct grid_usock *listener)
{
    size_t sz;
    grid_assert_state (self, GRID_AIPC_STATE_IDLE);

    /*  Take ownership of the listener socket. */
    self->listener = listener;
    self->listener_owner.src = GRID_AIPC_SRC_LISTENER;
    self->listener_owner.fsm = &self->fsm;
    grid_usock_swap_owner (listener, &self->listener_owner);

#if defined GRID_HAVE_WINDOWS
    /* Get/Set security attribute pointer*/
    grid_epbase_getopt (self->epbase, GRID_IPC, GRID_IPC_SEC_ATTR, &self->usock.sec_attr, &sz);

    grid_epbase_getopt (self->epbase, GRID_IPC, GRID_IPC_OUTBUFSZ, &self->usock.outbuffersz, &sz);
    grid_epbase_getopt (self->epbase, GRID_IPC, GRID_IPC_INBUFSZ, &self->usock.inbuffersz, &sz);
#endif

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);
}

void grid_aipc_stop (struct grid_aipc *self)
{
    grid_fsm_stop (&self->fsm);
}

static void grid_aipc_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_aipc *aipc;

    aipc = grid_cont (self, struct grid_aipc, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        if (!grid_sipc_isidle (&aipc->sipc)) {
            grid_epbase_stat_increment (aipc->epbase,
                GRID_STAT_DROPPED_CONNECTIONS, 1);
            grid_sipc_stop (&aipc->sipc);
        }
        aipc->state = GRID_AIPC_STATE_STOPPING_SIPC_FINAL;
    }
    if (grid_slow (aipc->state == GRID_AIPC_STATE_STOPPING_SIPC_FINAL)) {
        if (!grid_sipc_isidle (&aipc->sipc))
            return;
        grid_usock_stop (&aipc->usock);
        aipc->state = GRID_AIPC_STATE_STOPPING;
    }
    if (grid_slow (aipc->state == GRID_AIPC_STATE_STOPPING)) {
        if (!grid_usock_isidle (&aipc->usock))
            return;
       if (aipc->listener) {
            grid_assert (aipc->listener_owner.fsm);
            grid_usock_swap_owner (aipc->listener, &aipc->listener_owner);
            aipc->listener = NULL;
            aipc->listener_owner.src = -1;
            aipc->listener_owner.fsm = NULL;
        }
        aipc->state = GRID_AIPC_STATE_IDLE;
        grid_fsm_stopped (&aipc->fsm, GRID_AIPC_STOPPED);
        return;
    }

    grid_fsm_bad_state(aipc->state, src, type);
}

static void grid_aipc_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_aipc *aipc;
    int val;
    size_t sz;

    aipc = grid_cont (self, struct grid_aipc, fsm);

    switch (aipc->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  The state machine wasn't yet started.                                     */
/******************************************************************************/
    case GRID_AIPC_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_usock_accept (&aipc->usock, aipc->listener);
                aipc->state = GRID_AIPC_STATE_ACCEPTING;
                return;
            default:
                grid_fsm_bad_action (aipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (aipc->state, src, type);
        }

/******************************************************************************/
/*  ACCEPTING state.                                                          */
/*  Waiting for incoming connection.                                          */
/******************************************************************************/
    case GRID_AIPC_STATE_ACCEPTING:
        switch (src) {

        case GRID_AIPC_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_ACCEPTED:
                grid_epbase_clear_error (aipc->epbase);

                /*  Set the relevant socket options. */
                sz = sizeof (val);
                grid_epbase_getopt (aipc->epbase, GRID_SOL_SOCKET, GRID_SNDBUF,
                    &val, &sz);
                grid_assert (sz == sizeof (val));
                grid_usock_setsockopt (&aipc->usock, SOL_SOCKET, SO_SNDBUF,
                    &val, sizeof (val));
                sz = sizeof (val);
                grid_epbase_getopt (aipc->epbase, GRID_SOL_SOCKET, GRID_RCVBUF,
                    &val, &sz);
                grid_assert (sz == sizeof (val));
                grid_usock_setsockopt (&aipc->usock, SOL_SOCKET, SO_RCVBUF,
                    &val, sizeof (val));

                /*  Return ownership of the listening socket to the parent. */
                grid_usock_swap_owner (aipc->listener, &aipc->listener_owner);
                aipc->listener = NULL;
                aipc->listener_owner.src = -1;
                aipc->listener_owner.fsm = NULL;
                grid_fsm_raise (&aipc->fsm, &aipc->accepted, GRID_AIPC_ACCEPTED);

                /*  Start the sipc state machine. */
                grid_usock_activate (&aipc->usock);
                grid_sipc_start (&aipc->sipc, &aipc->usock);
                aipc->state = GRID_AIPC_STATE_ACTIVE;

                grid_epbase_stat_increment (aipc->epbase,
                    GRID_STAT_ACCEPTED_CONNECTIONS, 1);

                return;

            default:
                grid_fsm_bad_action (aipc->state, src, type);
            }

        case GRID_AIPC_SRC_LISTENER:
            switch (type) {
            case GRID_USOCK_ACCEPT_ERROR:
                grid_epbase_set_error (aipc->epbase,
                    grid_usock_geterrno (aipc->listener));
                grid_epbase_stat_increment (aipc->epbase,
                    GRID_STAT_ACCEPT_ERRORS, 1);
                grid_usock_accept (&aipc->usock, aipc->listener);

                return;

            default:
                grid_fsm_bad_action (aipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (aipc->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case GRID_AIPC_STATE_ACTIVE:
        switch (src) {

        case GRID_AIPC_SRC_SIPC:
            switch (type) {
            case GRID_SIPC_ERROR:
                grid_sipc_stop (&aipc->sipc);
                aipc->state = GRID_AIPC_STATE_STOPPING_SIPC;
                grid_epbase_stat_increment (aipc->epbase,
                    GRID_STAT_BROKEN_CONNECTIONS, 1);
                return;
            default:
                grid_fsm_bad_action (aipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (aipc->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_SIPC state.                                                      */
/******************************************************************************/
    case GRID_AIPC_STATE_STOPPING_SIPC:
        switch (src) {

        case GRID_AIPC_SRC_SIPC:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_SIPC_STOPPED:
                grid_usock_stop (&aipc->usock);
                aipc->state = GRID_AIPC_STATE_STOPPING_USOCK;
                return;
            default:
                grid_fsm_bad_action (aipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (aipc->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_USOCK state.                                                      */
/******************************************************************************/
    case GRID_AIPC_STATE_STOPPING_USOCK:
        switch (src) {

        case GRID_AIPC_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_USOCK_STOPPED:
                grid_fsm_raise (&aipc->fsm, &aipc->done, GRID_AIPC_ERROR);
                aipc->state = GRID_AIPC_STATE_DONE;
                return;
            default:
                grid_fsm_bad_action (aipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (aipc->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (aipc->state, src, type);
    }
}

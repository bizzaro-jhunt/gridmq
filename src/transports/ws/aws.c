/*
    Copyright (c) 2012-2013 250bpm s.r.o.  All rights reserved.
    Copyright (c) 2014 Wirebird Labs LLC.  All rights reserved.
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

#include "aws.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/attr.h"
#include "../../ws.h"

#define GRID_AWS_STATE_IDLE 1
#define GRID_AWS_STATE_ACCEPTING 2
#define GRID_AWS_STATE_ACTIVE 3
#define GRID_AWS_STATE_STOPPING_SWS 4
#define GRID_AWS_STATE_STOPPING_USOCK 5
#define GRID_AWS_STATE_DONE 6
#define GRID_AWS_STATE_STOPPING_SWS_FINAL 7
#define GRID_AWS_STATE_STOPPING 8

#define GRID_AWS_SRC_USOCK 1
#define GRID_AWS_SRC_SWS 2
#define GRID_AWS_SRC_LISTENER 3

/*  Private functions. */
static void grid_aws_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_aws_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);

void grid_aws_init (struct grid_aws *self, int src,
    struct grid_epbase *epbase, struct grid_fsm *owner)
{
    grid_fsm_init (&self->fsm, grid_aws_handler, grid_aws_shutdown,
        src, self, owner);
    self->state = GRID_AWS_STATE_IDLE;
    self->epbase = epbase;
    grid_usock_init (&self->usock, GRID_AWS_SRC_USOCK, &self->fsm);
    self->listener = NULL;
    self->listener_owner.src = -1;
    self->listener_owner.fsm = NULL;
    grid_sws_init (&self->sws, GRID_AWS_SRC_SWS, epbase, &self->fsm);
    grid_fsm_event_init (&self->accepted);
    grid_fsm_event_init (&self->done);
    grid_list_item_init (&self->item);
}

void grid_aws_term (struct grid_aws *self)
{
    grid_assert_state (self, GRID_AWS_STATE_IDLE);

    grid_list_item_term (&self->item);
    grid_fsm_event_term (&self->done);
    grid_fsm_event_term (&self->accepted);
    grid_sws_term (&self->sws);
    grid_usock_term (&self->usock);
    grid_fsm_term (&self->fsm);
}

int grid_aws_isidle (struct grid_aws *self)
{
    return grid_fsm_isidle (&self->fsm);
}

void grid_aws_start (struct grid_aws *self, struct grid_usock *listener)
{
    grid_assert_state (self, GRID_AWS_STATE_IDLE);

    /*  Take ownership of the listener socket. */
    self->listener = listener;
    self->listener_owner.src = GRID_AWS_SRC_LISTENER;
    self->listener_owner.fsm = &self->fsm;
    grid_usock_swap_owner (listener, &self->listener_owner);

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);
}

void grid_aws_stop (struct grid_aws *self)
{
    grid_fsm_stop (&self->fsm);
}

static void grid_aws_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_aws *aws;

    aws = grid_cont (self, struct grid_aws, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        if (!grid_sws_isidle (&aws->sws)) {
            grid_epbase_stat_increment (aws->epbase,
                GRID_STAT_DROPPED_CONNECTIONS, 1);
            grid_sws_stop (&aws->sws);
        }
        aws->state = GRID_AWS_STATE_STOPPING_SWS_FINAL;
    }
    if (grid_slow (aws->state == GRID_AWS_STATE_STOPPING_SWS_FINAL)) {
        if (!grid_sws_isidle (&aws->sws))
            return;
        grid_usock_stop (&aws->usock);
        aws->state = GRID_AWS_STATE_STOPPING;
    }
    if (grid_slow (aws->state == GRID_AWS_STATE_STOPPING)) {
        if (!grid_usock_isidle (&aws->usock))
            return;
       if (aws->listener) {
            grid_assert (aws->listener_owner.fsm);
            grid_usock_swap_owner (aws->listener, &aws->listener_owner);
            aws->listener = NULL;
            aws->listener_owner.src = -1;
            aws->listener_owner.fsm = NULL;
        }
        aws->state = GRID_AWS_STATE_IDLE;
        grid_fsm_stopped (&aws->fsm, GRID_AWS_STOPPED);
        return;
    }

    grid_fsm_bad_action (aws->state, src, type);
}

static void grid_aws_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_aws *aws;
    int val;
    size_t sz;
    uint8_t msg_type;

    aws = grid_cont (self, struct grid_aws, fsm);

    switch (aws->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  The state machine wasn't yet started.                                     */
/******************************************************************************/
    case GRID_AWS_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_usock_accept (&aws->usock, aws->listener);
                aws->state = GRID_AWS_STATE_ACCEPTING;
                return;
            default:
                grid_fsm_bad_action (aws->state, src, type);
            }

        default:
            grid_fsm_bad_source (aws->state, src, type);
        }

/******************************************************************************/
/*  ACCEPTING state.                                                          */
/*  Waiting for incoming connection.                                          */
/******************************************************************************/
    case GRID_AWS_STATE_ACCEPTING:
        switch (src) {

        case GRID_AWS_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_ACCEPTED:
                grid_epbase_clear_error (aws->epbase);

                /*  Set the relevant socket options. */
                sz = sizeof (val);
                grid_epbase_getopt (aws->epbase, GRID_SOL_SOCKET, GRID_SNDBUF,
                    &val, &sz);
                grid_assert (sz == sizeof (val));
                grid_usock_setsockopt (&aws->usock, SOL_SOCKET, SO_SNDBUF,
                    &val, sizeof (val));
                sz = sizeof (val);
                grid_epbase_getopt (aws->epbase, GRID_SOL_SOCKET, GRID_RCVBUF,
                    &val, &sz);
                grid_assert (sz == sizeof (val));
                grid_usock_setsockopt (&aws->usock, SOL_SOCKET, SO_RCVBUF,
                    &val, sizeof (val));
                sz = sizeof (val);
                grid_epbase_getopt (aws->epbase, GRID_WS, GRID_WS_MSG_TYPE,
                    &val, &sz);
                msg_type = (uint8_t)val;

                /*   Since the WebSocket handshake must poll, the receive
                     timeout is set to zero. Later, it will be set again
                     to the value specified by the socket option. */
                val = 0;
                sz = sizeof (val);
                grid_usock_setsockopt (&aws->usock, SOL_SOCKET, SO_RCVTIMEO,
                    &val, sizeof (val));

                /*  Return ownership of the listening socket to the parent. */
                grid_usock_swap_owner (aws->listener, &aws->listener_owner);
                aws->listener = NULL;
                aws->listener_owner.src = -1;
                aws->listener_owner.fsm = NULL;
                grid_fsm_raise (&aws->fsm, &aws->accepted, GRID_AWS_ACCEPTED);

                /*  Start the sws state machine. */
                grid_usock_activate (&aws->usock);
                grid_sws_start (&aws->sws, &aws->usock, GRID_WS_SERVER,
                    NULL, NULL, msg_type);
                aws->state = GRID_AWS_STATE_ACTIVE;

                grid_epbase_stat_increment (aws->epbase,
                    GRID_STAT_ACCEPTED_CONNECTIONS, 1);

                return;

            default:
                grid_fsm_bad_action (aws->state, src, type);
            }

        case GRID_AWS_SRC_LISTENER:
            switch (type) {

            case GRID_USOCK_ACCEPT_ERROR:
                grid_epbase_set_error (aws->epbase,
                    grid_usock_geterrno (aws->listener));
                grid_epbase_stat_increment (aws->epbase,
                    GRID_STAT_ACCEPT_ERRORS, 1);
                grid_usock_accept (&aws->usock, aws->listener);
                return;

            default:
                grid_fsm_bad_action (aws->state, src, type);
            }

        default:
            grid_fsm_bad_source (aws->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case GRID_AWS_STATE_ACTIVE:
        switch (src) {

        case GRID_AWS_SRC_SWS:
            switch (type) {
            case GRID_SWS_RETURN_CLOSE_HANDSHAKE:
                /*  Peer closed connection without intention to reconnect, or
                    local endpoint failed remote because of invalid data. */
                grid_sws_stop (&aws->sws);
                aws->state = GRID_AWS_STATE_STOPPING_SWS;
                return;
            case GRID_SWS_RETURN_ERROR:
                grid_sws_stop (&aws->sws);
                aws->state = GRID_AWS_STATE_STOPPING_SWS;
                grid_epbase_stat_increment (aws->epbase,
                    GRID_STAT_BROKEN_CONNECTIONS, 1);
                return;
            default:
                grid_fsm_bad_action (aws->state, src, type);
            }

        default:
            grid_fsm_bad_source (aws->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_SWS state.                                                       */
/******************************************************************************/
    case GRID_AWS_STATE_STOPPING_SWS:
        switch (src) {

        case GRID_AWS_SRC_SWS:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_SWS_RETURN_STOPPED:
                grid_usock_stop (&aws->usock);
                aws->state = GRID_AWS_STATE_STOPPING_USOCK;
                return;
            default:
                grid_fsm_bad_action (aws->state, src, type);
            }

        default:
            grid_fsm_bad_source (aws->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_USOCK state.                                                     */
/******************************************************************************/
    case GRID_AWS_STATE_STOPPING_USOCK:
        switch (src) {

        case GRID_AWS_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_USOCK_STOPPED:
                grid_aws_stop (aws);
                return;
            default:
                grid_fsm_bad_action (aws->state, src, type);
            }

        default:
            grid_fsm_bad_source (aws->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (aws->state, src, type);
    }
}


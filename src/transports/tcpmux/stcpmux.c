/*
    Copyright (c) 2013-2014 Martin Sustrik  All rights reserved.

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

#include "stcpmux.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/wire.h"
#include "../../utils/int.h"
#include "../../utils/attr.h"

/*  States of the object as a whole. */
#define GRID_STCPMUX_STATE_IDLE 1
#define GRID_STCPMUX_STATE_PROTOHDR 2
#define GRID_STCPMUX_STATE_STOPPING_STREAMHDR 3
#define GRID_STCPMUX_STATE_ACTIVE 4
#define GRID_STCPMUX_STATE_SHUTTING_DOWN 5
#define GRID_STCPMUX_STATE_DONE 6
#define GRID_STCPMUX_STATE_STOPPING 7

/*  Possible states of the inbound part of the object. */
#define GRID_STCPMUX_INSTATE_HDR 1
#define GRID_STCPMUX_INSTATE_BODY 2
#define GRID_STCPMUX_INSTATE_HASMSG 3

/*  Possible states of the outbound part of the object. */
#define GRID_STCPMUX_OUTSTATE_IDLE 1
#define GRID_STCPMUX_OUTSTATE_SENDING 2

/*  Subordinate srcptr objects. */
#define GRID_STCPMUX_SRC_USOCK 1
#define GRID_STCPMUX_SRC_STREAMHDR 2

/*  Stream is a special type of pipe. Implementation of the virtual pipe API. */
static int grid_stcpmux_send (struct grid_pipebase *self, struct grid_msg *msg);
static int grid_stcpmux_recv (struct grid_pipebase *self, struct grid_msg *msg);
const struct grid_pipebase_vfptr grid_stcpmux_pipebase_vfptr = {
    grid_stcpmux_send,
    grid_stcpmux_recv
};

/*  Private functions. */
static void grid_stcpmux_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_stcpmux_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);

void grid_stcpmux_init (struct grid_stcpmux *self, int src,
    struct grid_epbase *epbase, struct grid_fsm *owner)
{
    grid_fsm_init (&self->fsm, grid_stcpmux_handler, grid_stcpmux_shutdown,
        src, self, owner);
    self->state = GRID_STCPMUX_STATE_IDLE;
    grid_streamhdr_init (&self->streamhdr, GRID_STCPMUX_SRC_STREAMHDR, &self->fsm);
    self->usock = NULL;
    self->usock_owner.src = -1;
    self->usock_owner.fsm = NULL;
    grid_pipebase_init (&self->pipebase, &grid_stcpmux_pipebase_vfptr, epbase);
    self->instate = -1;
    grid_msg_init (&self->inmsg, 0);
    self->outstate = -1;
    grid_msg_init (&self->outmsg, 0);
    grid_fsm_event_init (&self->done);
}

void grid_stcpmux_term (struct grid_stcpmux *self)
{
    grid_assert_state (self, GRID_STCPMUX_STATE_IDLE);

    grid_fsm_event_term (&self->done);
    grid_msg_term (&self->outmsg);
    grid_msg_term (&self->inmsg);
    grid_pipebase_term (&self->pipebase);
    grid_streamhdr_term (&self->streamhdr);
    grid_fsm_term (&self->fsm);
}

int grid_stcpmux_isidle (struct grid_stcpmux *self)
{
    return grid_fsm_isidle (&self->fsm);
}

void grid_stcpmux_start (struct grid_stcpmux *self, struct grid_usock *usock)
{
    /*  Take ownership of the underlying socket. */
    grid_assert (self->usock == NULL && self->usock_owner.fsm == NULL);
    self->usock_owner.src = GRID_STCPMUX_SRC_USOCK;
    self->usock_owner.fsm = &self->fsm;
    grid_usock_swap_owner (usock, &self->usock_owner);
    self->usock = usock;

    /*  Launch the state machine. */
    grid_fsm_start (&self->fsm);
}

void grid_stcpmux_stop (struct grid_stcpmux *self)
{
    grid_fsm_stop (&self->fsm);
}

static int grid_stcpmux_send (struct grid_pipebase *self, struct grid_msg *msg)
{
    struct grid_stcpmux *stcpmux;
    struct grid_iovec iov [3];

    stcpmux = grid_cont (self, struct grid_stcpmux, pipebase);

    grid_assert_state (stcpmux, GRID_STCPMUX_STATE_ACTIVE);
    grid_assert (stcpmux->outstate == GRID_STCPMUX_OUTSTATE_IDLE);

    /*  Move the message to the local storage. */
    grid_msg_term (&stcpmux->outmsg);
    grid_msg_mv (&stcpmux->outmsg, msg);

    /*  Serialise the message header. */
    grid_putll (stcpmux->outhdr, grid_chunkref_size (&stcpmux->outmsg.sphdr) +
        grid_chunkref_size (&stcpmux->outmsg.body));

    /*  Start async sending. */
    iov [0].iov_base = stcpmux->outhdr;
    iov [0].iov_len = sizeof (stcpmux->outhdr);
    iov [1].iov_base = grid_chunkref_data (&stcpmux->outmsg.sphdr);
    iov [1].iov_len = grid_chunkref_size (&stcpmux->outmsg.sphdr);
    iov [2].iov_base = grid_chunkref_data (&stcpmux->outmsg.body);
    iov [2].iov_len = grid_chunkref_size (&stcpmux->outmsg.body);
    grid_usock_send (stcpmux->usock, iov, 3);

    stcpmux->outstate = GRID_STCPMUX_OUTSTATE_SENDING;

    return 0;
}

static int grid_stcpmux_recv (struct grid_pipebase *self, struct grid_msg *msg)
{
    struct grid_stcpmux *stcpmux;

    stcpmux = grid_cont (self, struct grid_stcpmux, pipebase);

    grid_assert_state (stcpmux, GRID_STCPMUX_STATE_ACTIVE);
    grid_assert (stcpmux->instate == GRID_STCPMUX_INSTATE_HASMSG);

    /*  Move received message to the user. */
    grid_msg_mv (msg, &stcpmux->inmsg);
    grid_msg_init (&stcpmux->inmsg, 0);

    /*  Start receiving new message. */
    stcpmux->instate = GRID_STCPMUX_INSTATE_HDR;
    grid_usock_recv (stcpmux->usock, stcpmux->inhdr, sizeof (stcpmux->inhdr),
        NULL);

    return 0;
}

static void grid_stcpmux_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_stcpmux *stcpmux;

    stcpmux = grid_cont (self, struct grid_stcpmux, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        grid_pipebase_stop (&stcpmux->pipebase);
        grid_streamhdr_stop (&stcpmux->streamhdr);
        stcpmux->state = GRID_STCPMUX_STATE_STOPPING;
    }
    if (grid_slow (stcpmux->state == GRID_STCPMUX_STATE_STOPPING)) {
        if (grid_streamhdr_isidle (&stcpmux->streamhdr)) {
            grid_usock_swap_owner (stcpmux->usock, &stcpmux->usock_owner);
            stcpmux->usock = NULL;
            stcpmux->usock_owner.src = -1;
            stcpmux->usock_owner.fsm = NULL;
            stcpmux->state = GRID_STCPMUX_STATE_IDLE;
            grid_fsm_stopped (&stcpmux->fsm, GRID_STCPMUX_STOPPED);
            return;
        }
        return;
    }

    grid_fsm_bad_state(stcpmux->state, src, type);
}

static void grid_stcpmux_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    int rc;
    struct grid_stcpmux *stcpmux;
    uint64_t size;

    stcpmux = grid_cont (self, struct grid_stcpmux, fsm);

    switch (stcpmux->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case GRID_STCPMUX_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_streamhdr_start (&stcpmux->streamhdr, stcpmux->usock,
                    &stcpmux->pipebase);
                stcpmux->state = GRID_STCPMUX_STATE_PROTOHDR;
                return;
            default:
                grid_fsm_bad_action (stcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (stcpmux->state, src, type);
        }

/******************************************************************************/
/*  PROTOHDR state.                                                           */
/******************************************************************************/
    case GRID_STCPMUX_STATE_PROTOHDR:
        switch (src) {

        case GRID_STCPMUX_SRC_STREAMHDR:
            switch (type) {
            case GRID_STREAMHDR_OK:

                /*  Before moving to the active state stop the streamhdr
                    state machine. */
                grid_streamhdr_stop (&stcpmux->streamhdr);
                stcpmux->state = GRID_STCPMUX_STATE_STOPPING_STREAMHDR;
                return;

            case GRID_STREAMHDR_ERROR:

                /* Raise the error and move directly to the DONE state.
                   streamhdr object will be stopped later on. */
                stcpmux->state = GRID_STCPMUX_STATE_DONE;
                grid_fsm_raise (&stcpmux->fsm, &stcpmux->done, GRID_STCPMUX_ERROR);
                return;

            default:
                grid_fsm_bad_action (stcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (stcpmux->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_STREAMHDR state.                                                 */
/******************************************************************************/
    case GRID_STCPMUX_STATE_STOPPING_STREAMHDR:
        switch (src) {

        case GRID_STCPMUX_SRC_STREAMHDR:
            switch (type) {
            case GRID_STREAMHDR_STOPPED:

                 /*  Start the pipe. */
                 rc = grid_pipebase_start (&stcpmux->pipebase);
                 if (grid_slow (rc < 0)) {
                    stcpmux->state = GRID_STCPMUX_STATE_DONE;
                    grid_fsm_raise (&stcpmux->fsm, &stcpmux->done,
                        GRID_STCPMUX_ERROR);
                    return;
                 }

                 /*  Start receiving a message in asynchronous manner. */
                 stcpmux->instate = GRID_STCPMUX_INSTATE_HDR;
                 grid_usock_recv (stcpmux->usock, &stcpmux->inhdr,
                     sizeof (stcpmux->inhdr), NULL);

                 /*  Mark the pipe as available for sending. */
                 stcpmux->outstate = GRID_STCPMUX_OUTSTATE_IDLE;

                 stcpmux->state = GRID_STCPMUX_STATE_ACTIVE;
                 return;

            default:
                grid_fsm_bad_action (stcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (stcpmux->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case GRID_STCPMUX_STATE_ACTIVE:
        switch (src) {

        case GRID_STCPMUX_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SENT:

                /*  The message is now fully sent. */
                grid_assert (stcpmux->outstate == GRID_STCPMUX_OUTSTATE_SENDING);
                stcpmux->outstate = GRID_STCPMUX_OUTSTATE_IDLE;
                grid_msg_term (&stcpmux->outmsg);
                grid_msg_init (&stcpmux->outmsg, 0);
                grid_pipebase_sent (&stcpmux->pipebase);
                return;

            case GRID_USOCK_RECEIVED:

                switch (stcpmux->instate) {
                case GRID_STCPMUX_INSTATE_HDR:

                    /*  Message header was received. Allocate memory for the
                        message. */
                    size = grid_getll (stcpmux->inhdr);
                    grid_msg_term (&stcpmux->inmsg);
                    grid_msg_init (&stcpmux->inmsg, (size_t) size);

                    /*  Special case when size of the message body is 0. */
                    if (!size) {
                        stcpmux->instate = GRID_STCPMUX_INSTATE_HASMSG;
                        grid_pipebase_received (&stcpmux->pipebase);
                        return;
                    }

                    /*  Start receiving the message body. */
                    stcpmux->instate = GRID_STCPMUX_INSTATE_BODY;
                    grid_usock_recv (stcpmux->usock,
                        grid_chunkref_data (&stcpmux->inmsg.body),
                        (size_t) size, NULL);

                    return;

                case GRID_STCPMUX_INSTATE_BODY:

                    /*  Message body was received. Notify the owner that it
                        can receive it. */
                    stcpmux->instate = GRID_STCPMUX_INSTATE_HASMSG;
                    grid_pipebase_received (&stcpmux->pipebase);

                    return;

                default:
                    grid_fsm_error("Unexpected socket instate",
                        stcpmux->state, src, type);
                }

            case GRID_USOCK_SHUTDOWN:
                grid_pipebase_stop (&stcpmux->pipebase);
                stcpmux->state = GRID_STCPMUX_STATE_SHUTTING_DOWN;
                return;

            case GRID_USOCK_ERROR:
                grid_pipebase_stop (&stcpmux->pipebase);
                stcpmux->state = GRID_STCPMUX_STATE_DONE;
                grid_fsm_raise (&stcpmux->fsm, &stcpmux->done, GRID_STCPMUX_ERROR);
                return;

            default:
                grid_fsm_bad_action (stcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (stcpmux->state, src, type);
        }

/******************************************************************************/
/*  SHUTTING_DOWN state.                                                      */
/*  The underlying connection is closed. We are just waiting that underlying  */
/*  usock being closed                                                        */
/******************************************************************************/
    case GRID_STCPMUX_STATE_SHUTTING_DOWN:
        switch (src) {

        case GRID_STCPMUX_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_ERROR:
                stcpmux->state = GRID_STCPMUX_STATE_DONE;
                grid_fsm_raise (&stcpmux->fsm, &stcpmux->done, GRID_STCPMUX_ERROR);
                return;
            default:
                grid_fsm_bad_action (stcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (stcpmux->state, src, type);
        }


/******************************************************************************/
/*  DONE state.                                                               */
/*  The underlying connection is closed. There's nothing that can be done in  */
/*  this state except stopping the object.                                    */
/******************************************************************************/
    case GRID_STCPMUX_STATE_DONE:
        grid_fsm_bad_source (stcpmux->state, src, type);

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (stcpmux->state, src, type);
    }
}


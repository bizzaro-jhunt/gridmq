/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.

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

#include "stcp.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/wire.h"
#include "../../utils/int.h"
#include "../../utils/attr.h"

/*  States of the object as a whole. */
#define GRID_STCP_STATE_IDLE 1
#define GRID_STCP_STATE_PROTOHDR 2
#define GRID_STCP_STATE_STOPPING_STREAMHDR 3
#define GRID_STCP_STATE_ACTIVE 4
#define GRID_STCP_STATE_SHUTTING_DOWN 5
#define GRID_STCP_STATE_DONE 6
#define GRID_STCP_STATE_STOPPING 7

/*  Possible states of the inbound part of the object. */
#define GRID_STCP_INSTATE_HDR 1
#define GRID_STCP_INSTATE_BODY 2
#define GRID_STCP_INSTATE_HASMSG 3

/*  Possible states of the outbound part of the object. */
#define GRID_STCP_OUTSTATE_IDLE 1
#define GRID_STCP_OUTSTATE_SENDING 2

/*  Subordinate srcptr objects. */
#define GRID_STCP_SRC_USOCK 1
#define GRID_STCP_SRC_STREAMHDR 2

/*  Stream is a special type of pipe. Implementation of the virtual pipe API. */
static int grid_stcp_send (struct grid_pipebase *self, struct grid_msg *msg);
static int grid_stcp_recv (struct grid_pipebase *self, struct grid_msg *msg);
const struct grid_pipebase_vfptr grid_stcp_pipebase_vfptr = {
    grid_stcp_send,
    grid_stcp_recv
};

/*  Private functions. */
static void grid_stcp_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_stcp_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);

void grid_stcp_init (struct grid_stcp *self, int src,
    struct grid_epbase *epbase, struct grid_fsm *owner)
{
    grid_fsm_init (&self->fsm, grid_stcp_handler, grid_stcp_shutdown,
        src, self, owner);
    self->state = GRID_STCP_STATE_IDLE;
    grid_streamhdr_init (&self->streamhdr, GRID_STCP_SRC_STREAMHDR, &self->fsm);
    self->usock = NULL;
    self->usock_owner.src = -1;
    self->usock_owner.fsm = NULL;
    grid_pipebase_init (&self->pipebase, &grid_stcp_pipebase_vfptr, epbase);
    self->instate = -1;
    grid_msg_init (&self->inmsg, 0);
    self->outstate = -1;
    grid_msg_init (&self->outmsg, 0);
    grid_fsm_event_init (&self->done);
}

void grid_stcp_term (struct grid_stcp *self)
{
    grid_assert_state (self, GRID_STCP_STATE_IDLE);

    grid_fsm_event_term (&self->done);
    grid_msg_term (&self->outmsg);
    grid_msg_term (&self->inmsg);
    grid_pipebase_term (&self->pipebase);
    grid_streamhdr_term (&self->streamhdr);
    grid_fsm_term (&self->fsm);
}

int grid_stcp_isidle (struct grid_stcp *self)
{
    return grid_fsm_isidle (&self->fsm);
}

void grid_stcp_start (struct grid_stcp *self, struct grid_usock *usock)
{
    /*  Take ownership of the underlying socket. */
    grid_assert (self->usock == NULL && self->usock_owner.fsm == NULL);
    self->usock_owner.src = GRID_STCP_SRC_USOCK;
    self->usock_owner.fsm = &self->fsm;
    grid_usock_swap_owner (usock, &self->usock_owner);
    self->usock = usock;

    /*  Launch the state machine. */
    grid_fsm_start (&self->fsm);
}

void grid_stcp_stop (struct grid_stcp *self)
{
    grid_fsm_stop (&self->fsm);
}

static int grid_stcp_send (struct grid_pipebase *self, struct grid_msg *msg)
{
    struct grid_stcp *stcp;
    struct grid_iovec iov [3];

    stcp = grid_cont (self, struct grid_stcp, pipebase);

    grid_assert_state (stcp, GRID_STCP_STATE_ACTIVE);
    grid_assert (stcp->outstate == GRID_STCP_OUTSTATE_IDLE);

    /*  Move the message to the local storage. */
    grid_msg_term (&stcp->outmsg);
    grid_msg_mv (&stcp->outmsg, msg);

    /*  Serialise the message header. */
    grid_putll (stcp->outhdr, grid_chunkref_size (&stcp->outmsg.sphdr) +
        grid_chunkref_size (&stcp->outmsg.body));

    /*  Start async sending. */
    iov [0].iov_base = stcp->outhdr;
    iov [0].iov_len = sizeof (stcp->outhdr);
    iov [1].iov_base = grid_chunkref_data (&stcp->outmsg.sphdr);
    iov [1].iov_len = grid_chunkref_size (&stcp->outmsg.sphdr);
    iov [2].iov_base = grid_chunkref_data (&stcp->outmsg.body);
    iov [2].iov_len = grid_chunkref_size (&stcp->outmsg.body);
    grid_usock_send (stcp->usock, iov, 3);

    stcp->outstate = GRID_STCP_OUTSTATE_SENDING;

    return 0;
}

static int grid_stcp_recv (struct grid_pipebase *self, struct grid_msg *msg)
{
    struct grid_stcp *stcp;

    stcp = grid_cont (self, struct grid_stcp, pipebase);

    grid_assert_state (stcp, GRID_STCP_STATE_ACTIVE);
    grid_assert (stcp->instate == GRID_STCP_INSTATE_HASMSG);

    /*  Move received message to the user. */
    grid_msg_mv (msg, &stcp->inmsg);
    grid_msg_init (&stcp->inmsg, 0);

    /*  Start receiving new message. */
    stcp->instate = GRID_STCP_INSTATE_HDR;
    grid_usock_recv (stcp->usock, stcp->inhdr, sizeof (stcp->inhdr), NULL);

    return 0;
}

static void grid_stcp_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_stcp *stcp;

    stcp = grid_cont (self, struct grid_stcp, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        grid_pipebase_stop (&stcp->pipebase);
        grid_streamhdr_stop (&stcp->streamhdr);
        stcp->state = GRID_STCP_STATE_STOPPING;
    }
    if (grid_slow (stcp->state == GRID_STCP_STATE_STOPPING)) {
        if (grid_streamhdr_isidle (&stcp->streamhdr)) {
            grid_usock_swap_owner (stcp->usock, &stcp->usock_owner);
            stcp->usock = NULL;
            stcp->usock_owner.src = -1;
            stcp->usock_owner.fsm = NULL;
            stcp->state = GRID_STCP_STATE_IDLE;
            grid_fsm_stopped (&stcp->fsm, GRID_STCP_STOPPED);
            return;
        }
        return;
    }

    grid_fsm_bad_state(stcp->state, src, type);
}

static void grid_stcp_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    int rc;
    struct grid_stcp *stcp;
    uint64_t size;
    int opt;
    size_t opt_sz = sizeof (opt);

    stcp = grid_cont (self, struct grid_stcp, fsm);

    switch (stcp->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case GRID_STCP_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_streamhdr_start (&stcp->streamhdr, stcp->usock,
                    &stcp->pipebase);
                stcp->state = GRID_STCP_STATE_PROTOHDR;
                return;
            default:
                grid_fsm_bad_action (stcp->state, src, type);
            }

        default:
            grid_fsm_bad_source (stcp->state, src, type);
        }

/******************************************************************************/
/*  PROTOHDR state.                                                           */
/******************************************************************************/
    case GRID_STCP_STATE_PROTOHDR:
        switch (src) {

        case GRID_STCP_SRC_STREAMHDR:
            switch (type) {
            case GRID_STREAMHDR_OK:

                /*  Before moving to the active state stop the streamhdr
                    state machine. */
                grid_streamhdr_stop (&stcp->streamhdr);
                stcp->state = GRID_STCP_STATE_STOPPING_STREAMHDR;
                return;

            case GRID_STREAMHDR_ERROR:

                /* Raise the error and move directly to the DONE state.
                   streamhdr object will be stopped later on. */
                stcp->state = GRID_STCP_STATE_DONE;
                grid_fsm_raise (&stcp->fsm, &stcp->done, GRID_STCP_ERROR);
                return;

            default:
                grid_fsm_bad_action (stcp->state, src, type);
            }

        default:
            grid_fsm_bad_source (stcp->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_STREAMHDR state.                                                 */
/******************************************************************************/
    case GRID_STCP_STATE_STOPPING_STREAMHDR:
        switch (src) {

        case GRID_STCP_SRC_STREAMHDR:
            switch (type) {
            case GRID_STREAMHDR_STOPPED:

                 /*  Start the pipe. */
                 rc = grid_pipebase_start (&stcp->pipebase);
                 if (grid_slow (rc < 0)) {
                    stcp->state = GRID_STCP_STATE_DONE;
                    grid_fsm_raise (&stcp->fsm, &stcp->done, GRID_STCP_ERROR);
                    return;
                 }

                 /*  Start receiving a message in asynchronous manner. */
                 stcp->instate = GRID_STCP_INSTATE_HDR;
                 grid_usock_recv (stcp->usock, &stcp->inhdr,
                     sizeof (stcp->inhdr), NULL);

                 /*  Mark the pipe as available for sending. */
                 stcp->outstate = GRID_STCP_OUTSTATE_IDLE;

                 stcp->state = GRID_STCP_STATE_ACTIVE;
                 return;

            default:
                grid_fsm_bad_action (stcp->state, src, type);
            }

        default:
            grid_fsm_bad_source (stcp->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case GRID_STCP_STATE_ACTIVE:
        switch (src) {

        case GRID_STCP_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SENT:

                /*  The message is now fully sent. */
                grid_assert (stcp->outstate == GRID_STCP_OUTSTATE_SENDING);
                stcp->outstate = GRID_STCP_OUTSTATE_IDLE;
                grid_msg_term (&stcp->outmsg);
                grid_msg_init (&stcp->outmsg, 0);
                grid_pipebase_sent (&stcp->pipebase);
                return;

            case GRID_USOCK_RECEIVED:

                switch (stcp->instate) {
                case GRID_STCP_INSTATE_HDR:

                    /*  Message header was received. Check that message size
                        is acceptable by comparing with GRID_RCVMAXSIZE;
                        if it's too large, drop the connection. */
                    size = grid_getll (stcp->inhdr);

                    grid_pipebase_getopt (&stcp->pipebase, GRID_SOL_SOCKET,
                        GRID_RCVMAXSIZE, &opt, &opt_sz);

                    if (opt >= 0 && size > (unsigned)opt) {
                        stcp->state = GRID_STCP_STATE_DONE;
                        grid_fsm_raise (&stcp->fsm, &stcp->done, GRID_STCP_ERROR);
                        return;
                    }

                    /*  Allocate memory for the message. */
                    grid_msg_term (&stcp->inmsg);
                    grid_msg_init (&stcp->inmsg, (size_t) size);

                    /*  Special case when size of the message body is 0. */
                    if (!size) {
                        stcp->instate = GRID_STCP_INSTATE_HASMSG;
                        grid_pipebase_received (&stcp->pipebase);
                        return;
                    }

                    /*  Start receiving the message body. */
                    stcp->instate = GRID_STCP_INSTATE_BODY;
                    grid_usock_recv (stcp->usock,
                        grid_chunkref_data (&stcp->inmsg.body),
                       (size_t) size, NULL);

                    return;

                case GRID_STCP_INSTATE_BODY:

                    /*  Message body was received. Notify the owner that it
                        can receive it. */
                    stcp->instate = GRID_STCP_INSTATE_HASMSG;
                    grid_pipebase_received (&stcp->pipebase);

                    return;

                default:
                    grid_fsm_error("Unexpected socket instate",
                        stcp->state, src, type);
                }

            case GRID_USOCK_SHUTDOWN:
                grid_pipebase_stop (&stcp->pipebase);
                stcp->state = GRID_STCP_STATE_SHUTTING_DOWN;
                return;

            case GRID_USOCK_ERROR:
                grid_pipebase_stop (&stcp->pipebase);
                stcp->state = GRID_STCP_STATE_DONE;
                grid_fsm_raise (&stcp->fsm, &stcp->done, GRID_STCP_ERROR);
                return;

            default:
                grid_fsm_bad_action (stcp->state, src, type);
            }

        default:
            grid_fsm_bad_source (stcp->state, src, type);
        }

/******************************************************************************/
/*  SHUTTING_DOWN state.                                                      */
/*  The underlying connection is closed. We are just waiting that underlying  */
/*  usock being closed                                                        */
/******************************************************************************/
    case GRID_STCP_STATE_SHUTTING_DOWN:
        switch (src) {

        case GRID_STCP_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_ERROR:
                stcp->state = GRID_STCP_STATE_DONE;
                grid_fsm_raise (&stcp->fsm, &stcp->done, GRID_STCP_ERROR);
                return;
            default:
                grid_fsm_bad_action (stcp->state, src, type);
            }

        default:
            grid_fsm_bad_source (stcp->state, src, type);
        }


/******************************************************************************/
/*  DONE state.                                                               */
/*  The underlying connection is closed. There's nothing that can be done in  */
/*  this state except stopping the object.                                    */
/******************************************************************************/
    case GRID_STCP_STATE_DONE:
        grid_fsm_bad_source (stcp->state, src, type);

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (stcp->state, src, type);
    }
}


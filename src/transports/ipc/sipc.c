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

#include "sipc.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/wire.h"
#include "../../utils/int.h"
#include "../../utils/attr.h"

/*  Types of messages passed via IPC transport. */
#define GRID_SIPC_MSG_NORMAL 1
#define GRID_SIPC_MSG_SHMEM 2

/*  States of the object as a whole. */
#define GRID_SIPC_STATE_IDLE 1
#define GRID_SIPC_STATE_PROTOHDR 2
#define GRID_SIPC_STATE_STOPPING_STREAMHDR 3
#define GRID_SIPC_STATE_ACTIVE 4
#define GRID_SIPC_STATE_SHUTTING_DOWN 5
#define GRID_SIPC_STATE_DONE 6
#define GRID_SIPC_STATE_STOPPING 7

/*  Subordinated srcptr objects. */
#define GRID_SIPC_SRC_USOCK 1
#define GRID_SIPC_SRC_STREAMHDR 2

/*  Possible states of the inbound part of the object. */
#define GRID_SIPC_INSTATE_HDR 1
#define GRID_SIPC_INSTATE_BODY 2
#define GRID_SIPC_INSTATE_HASMSG 3

/*  Possible states of the outbound part of the object. */
#define GRID_SIPC_OUTSTATE_IDLE 1
#define GRID_SIPC_OUTSTATE_SENDING 2

/*  Stream is a special type of pipe. Implementation of the virtual pipe API. */
static int grid_sipc_send (struct grid_pipebase *self, struct grid_msg *msg);
static int grid_sipc_recv (struct grid_pipebase *self, struct grid_msg *msg);
const struct grid_pipebase_vfptr grid_sipc_pipebase_vfptr = {
    grid_sipc_send,
    grid_sipc_recv
};

/*  Private functions. */
static void grid_sipc_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_sipc_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);

void grid_sipc_init (struct grid_sipc *self, int src,
    struct grid_epbase *epbase, struct grid_fsm *owner)
{
    grid_fsm_init (&self->fsm, grid_sipc_handler, grid_sipc_shutdown,
        src, self, owner);
    self->state = GRID_SIPC_STATE_IDLE;
    grid_streamhdr_init (&self->streamhdr, GRID_SIPC_SRC_STREAMHDR, &self->fsm);
    self->usock = NULL;
    self->usock_owner.src = -1;
    self->usock_owner.fsm = NULL;
    grid_pipebase_init (&self->pipebase, &grid_sipc_pipebase_vfptr, epbase);
    self->instate = -1;
    grid_msg_init (&self->inmsg, 0);
    self->outstate = -1;
    grid_msg_init (&self->outmsg, 0);
    grid_fsm_event_init (&self->done);
}

void grid_sipc_term (struct grid_sipc *self)
{
    grid_assert_state (self, GRID_SIPC_STATE_IDLE);

    grid_fsm_event_term (&self->done);
    grid_msg_term (&self->outmsg);
    grid_msg_term (&self->inmsg);
    grid_pipebase_term (&self->pipebase);
    grid_streamhdr_term (&self->streamhdr);
    grid_fsm_term (&self->fsm);
}

int grid_sipc_isidle (struct grid_sipc *self)
{
    return grid_fsm_isidle (&self->fsm);
}

void grid_sipc_start (struct grid_sipc *self, struct grid_usock *usock)
{
    /*  Take ownership of the underlying socket. */
    grid_assert (self->usock == NULL && self->usock_owner.fsm == NULL);
    self->usock_owner.src = GRID_SIPC_SRC_USOCK;
    self->usock_owner.fsm = &self->fsm;
    grid_usock_swap_owner (usock, &self->usock_owner);
    self->usock = usock;

    /*  Launch the state machine. */
    grid_fsm_start (&self->fsm);
}

void grid_sipc_stop (struct grid_sipc *self)
{
    grid_fsm_stop (&self->fsm);
}

static int grid_sipc_send (struct grid_pipebase *self, struct grid_msg *msg)
{
    struct grid_sipc *sipc;
    struct grid_iovec iov [3];

    sipc = grid_cont (self, struct grid_sipc, pipebase);

    grid_assert_state (sipc, GRID_SIPC_STATE_ACTIVE);
    grid_assert (sipc->outstate == GRID_SIPC_OUTSTATE_IDLE);

    /*  Move the message to the local storage. */
    grid_msg_term (&sipc->outmsg);
    grid_msg_mv (&sipc->outmsg, msg);

    /*  Serialise the message header. */
    sipc->outhdr [0] = GRID_SIPC_MSG_NORMAL;
    grid_putll (sipc->outhdr + 1, grid_chunkref_size (&sipc->outmsg.sphdr) +
        grid_chunkref_size (&sipc->outmsg.body));

    /*  Start async sending. */
    iov [0].iov_base = sipc->outhdr;
    iov [0].iov_len = sizeof (sipc->outhdr);
    iov [1].iov_base = grid_chunkref_data (&sipc->outmsg.sphdr);
    iov [1].iov_len = grid_chunkref_size (&sipc->outmsg.sphdr);
    iov [2].iov_base = grid_chunkref_data (&sipc->outmsg.body);
    iov [2].iov_len = grid_chunkref_size (&sipc->outmsg.body);
    grid_usock_send (sipc->usock, iov, 3);

    sipc->outstate = GRID_SIPC_OUTSTATE_SENDING;

    return 0;
}

static int grid_sipc_recv (struct grid_pipebase *self, struct grid_msg *msg)
{
    struct grid_sipc *sipc;

    sipc = grid_cont (self, struct grid_sipc, pipebase);

    grid_assert_state (sipc, GRID_SIPC_STATE_ACTIVE);
    grid_assert (sipc->instate == GRID_SIPC_INSTATE_HASMSG);

    /*  Move received message to the user. */
    grid_msg_mv (msg, &sipc->inmsg);
    grid_msg_init (&sipc->inmsg, 0);

    /*  Start receiving new message. */
    sipc->instate = GRID_SIPC_INSTATE_HDR;
    grid_usock_recv (sipc->usock, sipc->inhdr, sizeof (sipc->inhdr), NULL);

    return 0;
}

static void grid_sipc_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_sipc *sipc;

    sipc = grid_cont (self, struct grid_sipc, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        grid_pipebase_stop (&sipc->pipebase);
        grid_streamhdr_stop (&sipc->streamhdr);
        sipc->state = GRID_SIPC_STATE_STOPPING;
    }
    if (grid_slow (sipc->state == GRID_SIPC_STATE_STOPPING)) {
        if (grid_streamhdr_isidle (&sipc->streamhdr)) {
            grid_usock_swap_owner (sipc->usock, &sipc->usock_owner);
            sipc->usock = NULL;
            sipc->usock_owner.src = -1;
            sipc->usock_owner.fsm = NULL;
            sipc->state = GRID_SIPC_STATE_IDLE;
            grid_fsm_stopped (&sipc->fsm, GRID_SIPC_STOPPED);
            return;
        }
        return;
    }

    grid_fsm_bad_state(sipc->state, src, type);
}

static void grid_sipc_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    int rc;
    struct grid_sipc *sipc;
    uint64_t size;

    sipc = grid_cont (self, struct grid_sipc, fsm);


    switch (sipc->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case GRID_SIPC_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_streamhdr_start (&sipc->streamhdr, sipc->usock,
                    &sipc->pipebase);
                sipc->state = GRID_SIPC_STATE_PROTOHDR;
                return;
            default:
                grid_fsm_bad_action (sipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (sipc->state, src, type);
        }

/******************************************************************************/
/*  PROTOHDR state.                                                           */
/******************************************************************************/
    case GRID_SIPC_STATE_PROTOHDR:
        switch (src) {

        case GRID_SIPC_SRC_STREAMHDR:
            switch (type) {
            case GRID_STREAMHDR_OK:

                /*  Before moving to the active state stop the streamhdr
                    state machine. */
                grid_streamhdr_stop (&sipc->streamhdr);
                sipc->state = GRID_SIPC_STATE_STOPPING_STREAMHDR;
                return;

            case GRID_STREAMHDR_ERROR:

                /* Raise the error and move directly to the DONE state.
                   streamhdr object will be stopped later on. */
                sipc->state = GRID_SIPC_STATE_DONE;
                grid_fsm_raise (&sipc->fsm, &sipc->done, GRID_SIPC_ERROR);
                return;

            default:
                grid_fsm_bad_action (sipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (sipc->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_STREAMHDR state.                                                 */
/******************************************************************************/
    case GRID_SIPC_STATE_STOPPING_STREAMHDR:
        switch (src) {

        case GRID_SIPC_SRC_STREAMHDR:
            switch (type) {
            case GRID_STREAMHDR_STOPPED:

                 /*  Start the pipe. */
                 rc = grid_pipebase_start (&sipc->pipebase);
                 if (grid_slow (rc < 0)) {
                    sipc->state = GRID_SIPC_STATE_DONE;
                    grid_fsm_raise (&sipc->fsm, &sipc->done, GRID_SIPC_ERROR);
                    return;
                 }

                 /*  Start receiving a message in asynchronous manner. */
                 sipc->instate = GRID_SIPC_INSTATE_HDR;
                 grid_usock_recv (sipc->usock, &sipc->inhdr,
                     sizeof (sipc->inhdr), NULL);

                 /*  Mark the pipe as available for sending. */
                 sipc->outstate = GRID_SIPC_OUTSTATE_IDLE;

                 sipc->state = GRID_SIPC_STATE_ACTIVE;
                 return;

            default:
                grid_fsm_bad_action (sipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (sipc->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case GRID_SIPC_STATE_ACTIVE:
        switch (src) {

        case GRID_SIPC_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SENT:

                /*  The message is now fully sent. */
                grid_assert (sipc->outstate == GRID_SIPC_OUTSTATE_SENDING);
                sipc->outstate = GRID_SIPC_OUTSTATE_IDLE;
                grid_msg_term (&sipc->outmsg);
                grid_msg_init (&sipc->outmsg, 0);
                grid_pipebase_sent (&sipc->pipebase);
                return;

            case GRID_USOCK_RECEIVED:

                switch (sipc->instate) {
                case GRID_SIPC_INSTATE_HDR:

                    /*  Message header was received. Allocate memory for the
                        message. */
                    grid_assert (sipc->inhdr [0] == GRID_SIPC_MSG_NORMAL);
                    size = grid_getll (sipc->inhdr + 1);
                    grid_msg_term (&sipc->inmsg);
                    grid_msg_init (&sipc->inmsg, (size_t) size);

                    /*  Special case when size of the message body is 0. */
                    if (!size) {
                        sipc->instate = GRID_SIPC_INSTATE_HASMSG;
                        grid_pipebase_received (&sipc->pipebase);
                        return;
                    }

                    /*  Start receiving the message body. */
                    sipc->instate = GRID_SIPC_INSTATE_BODY;
                    grid_usock_recv (sipc->usock,
                        grid_chunkref_data (&sipc->inmsg.body),
                        (size_t) size, NULL);

                    return;

                case GRID_SIPC_INSTATE_BODY:

                    /*  Message body was received. Notify the owner that it
                        can receive it. */
                    sipc->instate = GRID_SIPC_INSTATE_HASMSG;
                    grid_pipebase_received (&sipc->pipebase);

                    return;

                default:
                    grid_assert (0);
                }

            case GRID_USOCK_SHUTDOWN:
                grid_pipebase_stop (&sipc->pipebase);
                sipc->state = GRID_SIPC_STATE_SHUTTING_DOWN;
                return;

            case GRID_USOCK_ERROR:
                grid_pipebase_stop (&sipc->pipebase);
                sipc->state = GRID_SIPC_STATE_DONE;
                grid_fsm_raise (&sipc->fsm, &sipc->done, GRID_SIPC_ERROR);
                return;


            default:
                grid_fsm_bad_action (sipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (sipc->state, src, type);
        }

/******************************************************************************/
/*  SHUTTING_DOWN state.                                                      */
/*  The underlying connection is closed. We are just waiting that underlying  */
/*  usock being closed                                                        */
/******************************************************************************/
    case GRID_SIPC_STATE_SHUTTING_DOWN:
        switch (src) {

        case GRID_SIPC_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_ERROR:
                sipc->state = GRID_SIPC_STATE_DONE;
                grid_fsm_raise (&sipc->fsm, &sipc->done, GRID_SIPC_ERROR);
                return;
            default:
                grid_fsm_bad_action (sipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (sipc->state, src, type);
        }

/******************************************************************************/
/*  DONE state.                                                               */
/*  The underlying connection is closed. There's nothing that can be done in  */
/*  this state except stopping the object.                                    */
/******************************************************************************/
    case GRID_SIPC_STATE_DONE:
        grid_fsm_bad_source (sipc->state, src, type);


/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (sipc->state, src, type);
    }
}

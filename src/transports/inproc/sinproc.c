/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.
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

#include "sinproc.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/attr.h"

#include <stddef.h>

#define GRID_SINPROC_STATE_IDLE 1
#define GRID_SINPROC_STATE_CONNECTING 2
#define GRID_SINPROC_STATE_READY 3
#define GRID_SINPROC_STATE_ACTIVE 4
#define GRID_SINPROC_STATE_DISCONNECTED 5
#define GRID_SINPROC_STATE_STOPPING_PEER 6
#define GRID_SINPROC_STATE_STOPPING 7

#define GRID_SINPROC_ACTION_READY 1
#define GRID_SINPROC_ACTION_ACCEPTED 2

/*  Set when SENT event was sent to the peer but RECEIVED haven't been
    passed back yet. */
#define GRID_SINPROC_FLAG_SENDING 1

/*  Set when SENT event was received, but the new message cannot be written
    to the queue yet, i.e. RECEIVED event haven't been returned
    to the peer yet. */
#define GRID_SINPROC_FLAG_RECEIVING 2

/*  Private functions. */
static void grid_sinproc_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_sinproc_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);

static int grid_sinproc_send (struct grid_pipebase *self, struct grid_msg *msg);
static int grid_sinproc_recv (struct grid_pipebase *self, struct grid_msg *msg);
const struct grid_pipebase_vfptr grid_sinproc_pipebase_vfptr = {
    grid_sinproc_send,
    grid_sinproc_recv
};

void grid_sinproc_init (struct grid_sinproc *self, int src,
    struct grid_epbase *epbase, struct grid_fsm *owner)
{
    int rcvbuf;
    size_t sz;

    grid_fsm_init (&self->fsm, grid_sinproc_handler, grid_sinproc_shutdown,
        src, self, owner);
    self->state = GRID_SINPROC_STATE_IDLE;
    self->flags = 0;
    self->peer = NULL;
    grid_pipebase_init (&self->pipebase, &grid_sinproc_pipebase_vfptr, epbase);
    sz = sizeof (rcvbuf);
    grid_epbase_getopt (epbase, GRID_SOL_SOCKET, GRID_RCVBUF, &rcvbuf, &sz);
    grid_assert (sz == sizeof (rcvbuf));
    grid_msgqueue_init (&self->msgqueue, rcvbuf);
    grid_msg_init (&self->msg, 0);
    grid_fsm_event_init (&self->event_connect);
    grid_fsm_event_init (&self->event_sent);
    grid_fsm_event_init (&self->event_received);
    grid_fsm_event_init (&self->event_disconnect);
    grid_list_item_init (&self->item);
}

void grid_sinproc_term (struct grid_sinproc *self)
{
    grid_list_item_term (&self->item);
    grid_fsm_event_term (&self->event_disconnect);
    grid_fsm_event_term (&self->event_received);
    grid_fsm_event_term (&self->event_sent);
    grid_fsm_event_term (&self->event_connect);
    grid_msg_term (&self->msg);
    grid_msgqueue_term (&self->msgqueue);
    grid_pipebase_term (&self->pipebase);
    grid_fsm_term (&self->fsm);
}

int grid_sinproc_isidle (struct grid_sinproc *self)
{
    return grid_fsm_isidle (&self->fsm);
}

void grid_sinproc_connect (struct grid_sinproc *self, struct grid_fsm *peer)
{
    grid_fsm_start (&self->fsm);

    /*  Start the connecting handshake with the peer. */
    grid_fsm_raiseto (&self->fsm, peer, &self->event_connect,
        GRID_SINPROC_SRC_PEER, GRID_SINPROC_CONNECT, self);
}

void grid_sinproc_accept (struct grid_sinproc *self, struct grid_sinproc *peer)
{
    grid_assert (!self->peer);
    self->peer = peer;

    /*  Start the connecting handshake with the peer. */
    grid_fsm_raiseto (&self->fsm, &peer->fsm, &self->event_connect,
        GRID_SINPROC_SRC_PEER, GRID_SINPROC_READY, self);

    /*  Notify the state machine. */
    grid_fsm_start (&self->fsm);
    grid_fsm_action (&self->fsm, GRID_SINPROC_ACTION_READY);
}

void grid_sinproc_stop (struct grid_sinproc *self)
{
    grid_fsm_stop (&self->fsm);
}



static int grid_sinproc_send (struct grid_pipebase *self, struct grid_msg *msg)
{
    struct grid_sinproc *sinproc;
    struct grid_msg nmsg;

    sinproc = grid_cont (self, struct grid_sinproc, pipebase);

    /*  If the peer have already closed the connection, we cannot send
        anymore. */
    if (sinproc->state == GRID_SINPROC_STATE_DISCONNECTED)
        return -ECONNRESET;

    /*  Sanity checks. */
    grid_assert_state (sinproc, GRID_SINPROC_STATE_ACTIVE);
    grid_assert (!(sinproc->flags & GRID_SINPROC_FLAG_SENDING));

    grid_msg_init (&nmsg,
        grid_chunkref_size (&msg->sphdr) +
        grid_chunkref_size (&msg->body));
    memcpy (grid_chunkref_data (&nmsg.body),
        grid_chunkref_data (&msg->sphdr),
        grid_chunkref_size (&msg->sphdr));
    memcpy ((char *)grid_chunkref_data (&nmsg.body) +
        grid_chunkref_size (&msg->sphdr),
        grid_chunkref_data (&msg->body),
        grid_chunkref_size (&msg->body));
    grid_msg_term (msg);

    /*  Expose the message to the peer. */
    grid_msg_term (&sinproc->msg);
    grid_msg_mv (&sinproc->msg, &nmsg);

    /*  Notify the peer that there's a message to get. */
    sinproc->flags |= GRID_SINPROC_FLAG_SENDING;
    grid_fsm_raiseto (&sinproc->fsm, &sinproc->peer->fsm,
        &sinproc->peer->event_sent, GRID_SINPROC_SRC_PEER,
        GRID_SINPROC_SENT, sinproc);

    return 0;
}

static int grid_sinproc_recv (struct grid_pipebase *self, struct grid_msg *msg)
{
    int rc;
    struct grid_sinproc *sinproc;

    sinproc = grid_cont (self, struct grid_sinproc, pipebase);

    /*  Sanity check. */
    grid_assert (sinproc->state == GRID_SINPROC_STATE_ACTIVE ||
        sinproc->state == GRID_SINPROC_STATE_DISCONNECTED);

    /*  Move the message to the caller. */
    rc = grid_msgqueue_recv (&sinproc->msgqueue, msg);
    errnum_assert (rc == 0, -rc);

    /*  If there was a message from peer lingering because of the exceeded
        buffer limit, try to enqueue it once again. */
    if (sinproc->state != GRID_SINPROC_STATE_DISCONNECTED) {
        if (grid_slow (sinproc->flags & GRID_SINPROC_FLAG_RECEIVING)) {
            rc = grid_msgqueue_send (&sinproc->msgqueue, &sinproc->peer->msg);
            grid_assert (rc == 0 || rc == -EAGAIN);
            if (rc == 0) {
                errnum_assert (rc == 0, -rc);
                grid_msg_init (&sinproc->peer->msg, 0);
                grid_fsm_raiseto (&sinproc->fsm, &sinproc->peer->fsm,
                    &sinproc->peer->event_received, GRID_SINPROC_SRC_PEER,
                    GRID_SINPROC_RECEIVED, sinproc);
                sinproc->flags &= ~GRID_SINPROC_FLAG_RECEIVING;
            }
        }
    }

    if (!grid_msgqueue_empty (&sinproc->msgqueue))
       grid_pipebase_received (&sinproc->pipebase);

    return 0;
}

static void grid_sinproc_shutdown_events (struct grid_sinproc *self, int src,
    int type, GRID_UNUSED void *srcptr)
{
    /*  *******************************  */
    /*  Any-state events                 */
    /*  *******************************  */
    switch (src) {
    case GRID_FSM_ACTION:
        switch (type) {
        case GRID_FSM_STOP:
            if (self->state != GRID_SINPROC_STATE_IDLE &&
                  self->state != GRID_SINPROC_STATE_DISCONNECTED) {
                grid_pipebase_stop (&self->pipebase);
                grid_assert (self->fsm.state == 2 || self->fsm.state == 3);
                grid_fsm_raiseto (&self->fsm, &self->peer->fsm,
                    &self->peer->event_disconnect, GRID_SINPROC_SRC_PEER,
                    GRID_SINPROC_DISCONNECT, self);

                self->state = GRID_SINPROC_STATE_STOPPING_PEER;
            } else {
                self->state = GRID_SINPROC_STATE_STOPPING;
            }
            return;
        }
    case GRID_SINPROC_SRC_PEER:
        switch (type) {
        case GRID_SINPROC_RECEIVED:
            return;
        }
    }

    /*  *******************************  */
    /*  Regular events                   */
    /*  *******************************  */
    switch (self->state) {
    case GRID_SINPROC_STATE_STOPPING_PEER:
        switch (src) {
        case GRID_SINPROC_SRC_PEER:
            switch (type) {
            case GRID_SINPROC_DISCONNECT:
                self->state = GRID_SINPROC_STATE_STOPPING;
                return;
            default:
                grid_fsm_bad_action (self->state, src, type);
            }
        default:
            grid_fsm_bad_source (self->state, src, type);
        }
    default:
        grid_fsm_bad_state (self->state, src, type);
    }

    grid_fsm_bad_action (self->state, src, type);
}

static void grid_sinproc_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    struct grid_sinproc *sinproc;

    sinproc = grid_cont (self, struct grid_sinproc, fsm);
    grid_assert (sinproc->fsm.state == 3);

    grid_sinproc_shutdown_events (sinproc, src, type, srcptr);

    /*  ***************  */
    /*  States to check  */
    /*  ***************  */

    /*  Have we got notification that peer is stopped  */
    if (grid_slow (sinproc->state != GRID_SINPROC_STATE_STOPPING)) {
        return;
    }

    /*  Are all events processed? We can't cancel them unfortunately  */
    if (grid_fsm_event_active (&sinproc->event_received)
        || grid_fsm_event_active (&sinproc->event_disconnect))
    {
        return;
    }
    /*  These events are deemed to be impossible here  */
    grid_assert (!grid_fsm_event_active (&sinproc->event_connect));
    grid_assert (!grid_fsm_event_active (&sinproc->event_sent));

    /*  **********************************************  */
    /*  All checks are successful. Just stop right now  */
    /*  **********************************************  */

    grid_fsm_stopped (&sinproc->fsm, GRID_SINPROC_STOPPED);
    return;
}

static void grid_sinproc_handler (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    int rc;
    struct grid_sinproc *sinproc;
    int empty;

    sinproc = grid_cont (self, struct grid_sinproc, fsm);

    switch (sinproc->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case GRID_SINPROC_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                sinproc->state = GRID_SINPROC_STATE_CONNECTING;
                return;
            default:
                grid_fsm_bad_action (sinproc->state, src, type);
            }

        default:
            grid_fsm_bad_source (sinproc->state, src, type);
        }

/******************************************************************************/
/*  CONNECTING state.                                                         */
/*  CONNECT request was sent to the peer. Now we are waiting for the          */
/*  acknowledgement.                                                          */
/******************************************************************************/
    case GRID_SINPROC_STATE_CONNECTING:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_SINPROC_ACTION_READY:
                sinproc->state = GRID_SINPROC_STATE_READY;
                return;
            default:
                grid_fsm_bad_action (sinproc->state, src, type);
            }

        case GRID_SINPROC_SRC_PEER:
            switch (type) {
            case GRID_SINPROC_READY:
                sinproc->peer = (struct grid_sinproc*) srcptr;
                rc = grid_pipebase_start (&sinproc->pipebase);
                errnum_assert (rc == 0, -rc);
                sinproc->state = GRID_SINPROC_STATE_ACTIVE;
                grid_fsm_raiseto (&sinproc->fsm, &sinproc->peer->fsm,
                    &sinproc->event_connect,
                    GRID_SINPROC_SRC_PEER, GRID_SINPROC_ACCEPTED, self);
                return;
            default:
                grid_fsm_bad_action (sinproc->state, src, type);
            }

        default:
            grid_fsm_bad_source (sinproc->state, src, type);
        }

/******************************************************************************/
/*  READY state.                                                              */
/*                                                                            */
/******************************************************************************/
    case GRID_SINPROC_STATE_READY:
        switch (src) {

        case GRID_SINPROC_SRC_PEER:
            switch (type) {
            case GRID_SINPROC_READY:
                /*  This means both peers sent READY so they are both
                    ready for receiving messages  */
                rc = grid_pipebase_start (&sinproc->pipebase);
                errnum_assert (rc == 0, -rc);
                sinproc->state = GRID_SINPROC_STATE_ACTIVE;
                return;
            case GRID_SINPROC_ACCEPTED:
                rc = grid_pipebase_start (&sinproc->pipebase);
                errnum_assert (rc == 0, -rc);
                sinproc->state = GRID_SINPROC_STATE_ACTIVE;
                return;
            default:
                grid_fsm_bad_action (sinproc->state, src, type);
            }

        default:
            grid_fsm_bad_source (sinproc->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case GRID_SINPROC_STATE_ACTIVE:
        switch (src) {

        case GRID_SINPROC_SRC_PEER:
            switch (type) {
            case GRID_SINPROC_SENT:

                empty = grid_msgqueue_empty (&sinproc->msgqueue);

                /*  Push the message to the inbound message queue. */
                rc = grid_msgqueue_send (&sinproc->msgqueue,
                    &sinproc->peer->msg);
                if (rc == -EAGAIN) {
                    sinproc->flags |= GRID_SINPROC_FLAG_RECEIVING;
                    return;
                }
                errnum_assert (rc == 0, -rc);
                grid_msg_init (&sinproc->peer->msg, 0);

                /*  Notify the user that there's a message to receive. */
                if (empty)
                    grid_pipebase_received (&sinproc->pipebase);

                /*  Notify the peer that the message was received. */
                grid_fsm_raiseto (&sinproc->fsm, &sinproc->peer->fsm,
                    &sinproc->peer->event_received, GRID_SINPROC_SRC_PEER,
                    GRID_SINPROC_RECEIVED, sinproc);

                return;

            case GRID_SINPROC_RECEIVED:
                grid_assert (sinproc->flags & GRID_SINPROC_FLAG_SENDING);
                grid_pipebase_sent (&sinproc->pipebase);
                sinproc->flags &= ~GRID_SINPROC_FLAG_SENDING;
                return;

            case GRID_SINPROC_DISCONNECT:
                grid_pipebase_stop (&sinproc->pipebase);
                grid_fsm_raiseto (&sinproc->fsm, &sinproc->peer->fsm,
                    &sinproc->peer->event_disconnect, GRID_SINPROC_SRC_PEER,
                    GRID_SINPROC_DISCONNECT, sinproc);
                sinproc->state = GRID_SINPROC_STATE_DISCONNECTED;
                sinproc->peer = NULL;
                grid_fsm_raise (&sinproc->fsm, &sinproc->event_disconnect,
                    GRID_SINPROC_DISCONNECT);
                return;

            default:
                grid_fsm_bad_action (sinproc->state, src, type);
            }

        default:
            grid_fsm_bad_source (sinproc->state, src, type);
        }

/******************************************************************************/
/*  DISCONNECTED state.                                                       */
/*  The peer have already closed the connection, but the object was not yet   */
/*  asked to stop.                                                            */
/******************************************************************************/
    case GRID_SINPROC_STATE_DISCONNECTED:
        switch (src) {
        case GRID_SINPROC_SRC_PEER:
            switch (type) {
            case GRID_SINPROC_RECEIVED:
                /*  This case can safely be ignored. It may happen when
                    grid_close() comes before the already enqueued
                    GRID_SINPROC_RECEIVED has been delivered.  */
                return;
            default:
                grid_fsm_bad_action (sinproc->state, src, type);
            };
        default:
            grid_fsm_bad_source (sinproc->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (sinproc->state, src, type);
    }
}


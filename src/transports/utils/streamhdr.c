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

#include "streamhdr.h"

#include "../../aio/timer.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/wire.h"
#include "../../utils/attr.h"

#include <stddef.h>
#include <string.h>

#define GRID_STREAMHDR_STATE_IDLE 1
#define GRID_STREAMHDR_STATE_SENDING 2
#define GRID_STREAMHDR_STATE_RECEIVING 3
#define GRID_STREAMHDR_STATE_STOPPING_TIMER_ERROR 4
#define GRID_STREAMHDR_STATE_STOPPING_TIMER_DONE 5
#define GRID_STREAMHDR_STATE_DONE 6
#define GRID_STREAMHDR_STATE_STOPPING 7

#define GRID_STREAMHDR_SRC_USOCK 1
#define GRID_STREAMHDR_SRC_TIMER 2

/*  Private functions. */
static void grid_streamhdr_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_streamhdr_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);

void grid_streamhdr_init (struct grid_streamhdr *self, int src,
    struct grid_fsm *owner)
{
    grid_fsm_init (&self->fsm, grid_streamhdr_handler, grid_streamhdr_shutdown,
        src, self, owner);
    self->state = GRID_STREAMHDR_STATE_IDLE;
    grid_timer_init (&self->timer, GRID_STREAMHDR_SRC_TIMER, &self->fsm);
    grid_fsm_event_init (&self->done);

    self->usock = NULL;
    self->usock_owner.src = -1;
    self->usock_owner.fsm = NULL;
    self->pipebase = NULL;
}

void grid_streamhdr_term (struct grid_streamhdr *self)
{
    grid_assert_state (self, GRID_STREAMHDR_STATE_IDLE);

    grid_fsm_event_term (&self->done);
    grid_timer_term (&self->timer);
    grid_fsm_term (&self->fsm);
}

int grid_streamhdr_isidle (struct grid_streamhdr *self)
{
    return grid_fsm_isidle (&self->fsm);
}

void grid_streamhdr_start (struct grid_streamhdr *self, struct grid_usock *usock,
    struct grid_pipebase *pipebase)
{
    size_t sz;
    int protocol;

    /*  Take ownership of the underlying socket. */
    grid_assert (self->usock == NULL && self->usock_owner.fsm == NULL);
    self->usock_owner.src = GRID_STREAMHDR_SRC_USOCK;
    self->usock_owner.fsm = &self->fsm;
    grid_usock_swap_owner (usock, &self->usock_owner);
    self->usock = usock;
    self->pipebase = pipebase;

    /*  Get the protocol identifier. */
    sz = sizeof (protocol);
    grid_pipebase_getopt (pipebase, GRID_SOL_SOCKET, GRID_PROTOCOL, &protocol, &sz);
    grid_assert (sz == sizeof (protocol));

    /*  Compose the protocol header. */
    memcpy (self->protohdr, "\0SP\0\0\0\0\0", 8);
    grid_puts (self->protohdr + 4, (uint16_t) protocol);

    /*  Launch the state machine. */
    grid_fsm_start (&self->fsm);
}

void grid_streamhdr_stop (struct grid_streamhdr *self)
{
    grid_fsm_stop (&self->fsm);
}

static void grid_streamhdr_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_streamhdr *streamhdr;

    streamhdr = grid_cont (self, struct grid_streamhdr, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        grid_timer_stop (&streamhdr->timer);
        streamhdr->state = GRID_STREAMHDR_STATE_STOPPING;
    }
    if (grid_slow (streamhdr->state == GRID_STREAMHDR_STATE_STOPPING)) {
        if (!grid_timer_isidle (&streamhdr->timer))
            return;
        streamhdr->state = GRID_STREAMHDR_STATE_IDLE;
        grid_fsm_stopped (&streamhdr->fsm, GRID_STREAMHDR_STOPPED);
        return;
    }

    grid_fsm_bad_state (streamhdr->state, src, type);
}

static void grid_streamhdr_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_streamhdr *streamhdr;
    struct grid_iovec iovec;
    int protocol;

    streamhdr = grid_cont (self, struct grid_streamhdr, fsm);


    switch (streamhdr->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case GRID_STREAMHDR_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_timer_start (&streamhdr->timer, 1000);
                iovec.iov_base = streamhdr->protohdr;
                iovec.iov_len = sizeof (streamhdr->protohdr);
                grid_usock_send (streamhdr->usock, &iovec, 1);
                streamhdr->state = GRID_STREAMHDR_STATE_SENDING;
                return;
            default:
                grid_fsm_bad_action (streamhdr->state, src, type);
            }

        default:
            grid_fsm_bad_source (streamhdr->state, src, type);
        }

/******************************************************************************/
/*  SENDING state.                                                            */
/******************************************************************************/
    case GRID_STREAMHDR_STATE_SENDING:
        switch (src) {

        case GRID_STREAMHDR_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SENT:
                grid_usock_recv (streamhdr->usock, streamhdr->protohdr,
                    sizeof (streamhdr->protohdr), NULL);
                streamhdr->state = GRID_STREAMHDR_STATE_RECEIVING;
                return;
            case GRID_USOCK_SHUTDOWN:
                /*  Ignore it. Wait for ERROR event  */
                return;
            case GRID_USOCK_ERROR:
                grid_timer_stop (&streamhdr->timer);
                streamhdr->state = GRID_STREAMHDR_STATE_STOPPING_TIMER_ERROR;
                return;
            default:
                grid_fsm_bad_action (streamhdr->state, src, type);
            }

        case GRID_STREAMHDR_SRC_TIMER:
            switch (type) {
            case GRID_TIMER_TIMEOUT:
                grid_timer_stop (&streamhdr->timer);
                streamhdr->state = GRID_STREAMHDR_STATE_STOPPING_TIMER_ERROR;
                return;
            default:
                grid_fsm_bad_action (streamhdr->state, src, type);
            }

        default:
            grid_fsm_bad_source (streamhdr->state, src, type);
        }

/******************************************************************************/
/*  RECEIVING state.                                                          */
/******************************************************************************/
    case GRID_STREAMHDR_STATE_RECEIVING:
        switch (src) {

        case GRID_STREAMHDR_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_RECEIVED:

                /*  Here we are checking whether the peer speaks the same
                    protocol as this socket. */
                if (memcmp (streamhdr->protohdr, "\0SP\0", 4) != 0)
                    goto invalidhdr;
                protocol = grid_gets (streamhdr->protohdr + 4);
                if (!grid_pipebase_ispeer (streamhdr->pipebase, protocol))
                    goto invalidhdr;
                grid_timer_stop (&streamhdr->timer);
                streamhdr->state = GRID_STREAMHDR_STATE_STOPPING_TIMER_DONE;
                return;
            case GRID_USOCK_SHUTDOWN:
                /*  Ignore it. Wait for ERROR event  */
                return;
            case GRID_USOCK_ERROR:
invalidhdr:
                grid_timer_stop (&streamhdr->timer);
                streamhdr->state = GRID_STREAMHDR_STATE_STOPPING_TIMER_ERROR;
                return;
            default:
                grid_assert (0);
            }

        case GRID_STREAMHDR_SRC_TIMER:
            switch (type) {
            case GRID_TIMER_TIMEOUT:
                grid_timer_stop (&streamhdr->timer);
                streamhdr->state = GRID_STREAMHDR_STATE_STOPPING_TIMER_ERROR;
                return;
            default:
                grid_fsm_bad_action (streamhdr->state, src, type);
            }

        default:
            grid_fsm_bad_source (streamhdr->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_TIMER_ERROR state.                                               */
/******************************************************************************/
    case GRID_STREAMHDR_STATE_STOPPING_TIMER_ERROR:
        switch (src) {

        case GRID_STREAMHDR_SRC_USOCK:
            /*  It's safe to ignore usock event when we are stopping, but there
                is only a subset of events that are plausible. */
            grid_assert (type == GRID_USOCK_ERROR);
            return;

        case GRID_STREAMHDR_SRC_TIMER:
            switch (type) {
            case GRID_TIMER_STOPPED:
                grid_usock_swap_owner (streamhdr->usock, &streamhdr->usock_owner);
                streamhdr->usock = NULL;
                streamhdr->usock_owner.src = -1;
                streamhdr->usock_owner.fsm = NULL;
                streamhdr->state = GRID_STREAMHDR_STATE_DONE;
                grid_fsm_raise (&streamhdr->fsm, &streamhdr->done,
                    GRID_STREAMHDR_ERROR);
                return;
            default:
                grid_fsm_bad_action (streamhdr->state, src, type);
            }

        default:
            grid_fsm_bad_source (streamhdr->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_TIMER_DONE state.                                                */
/******************************************************************************/
    case GRID_STREAMHDR_STATE_STOPPING_TIMER_DONE:
        switch (src) {

        case GRID_STREAMHDR_SRC_USOCK:
            /*  It's safe to ignore usock event when we are stopping, but there
                is only a subset of events that are plausible. */
            grid_assert (type == GRID_USOCK_ERROR);
            return;

        case GRID_STREAMHDR_SRC_TIMER:
            switch (type) {
            case GRID_TIMER_STOPPED:
                grid_usock_swap_owner (streamhdr->usock, &streamhdr->usock_owner);
                streamhdr->usock = NULL;
                streamhdr->usock_owner.src = -1;
                streamhdr->usock_owner.fsm = NULL;
                streamhdr->state = GRID_STREAMHDR_STATE_DONE;
                grid_fsm_raise (&streamhdr->fsm, &streamhdr->done,
                    GRID_STREAMHDR_OK);
                return;
            default:
                grid_fsm_bad_action (streamhdr->state, src, type);
            }

        default:
            grid_fsm_bad_source (streamhdr->state, src, type);
        }

/******************************************************************************/
/*  DONE state.                                                               */
/*  The header exchange was either done successfully of failed. There's       */
/*  nothing that can be done in this state except stopping the object.        */
/******************************************************************************/
    case GRID_STREAMHDR_STATE_DONE:
        grid_fsm_bad_source (streamhdr->state, src, type);

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (streamhdr->state, src, type);
    }
}


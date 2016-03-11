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

#include "../transport.h"
#include "../protocol.h"

#include "sock.h"
#include "ep.h"

#include "../utils/err.h"
#include "../utils/fast.h"

/*  Internal pipe states. */
#define GRID_PIPEBASE_STATE_IDLE 1
#define GRID_PIPEBASE_STATE_ACTIVE 2
#define GRID_PIPEBASE_STATE_FAILED 3

#define GRID_PIPEBASE_INSTATE_DEACTIVATED 0
#define GRID_PIPEBASE_INSTATE_IDLE 1
#define GRID_PIPEBASE_INSTATE_RECEIVING 2
#define GRID_PIPEBASE_INSTATE_RECEIVED 3
#define GRID_PIPEBASE_INSTATE_ASYNC 4

#define GRID_PIPEBASE_OUTSTATE_DEACTIVATED 0
#define GRID_PIPEBASE_OUTSTATE_IDLE 1
#define GRID_PIPEBASE_OUTSTATE_SENDING 2
#define GRID_PIPEBASE_OUTSTATE_SENT 3
#define GRID_PIPEBASE_OUTSTATE_ASYNC 4

void grid_pipebase_init (struct grid_pipebase *self,
    const struct grid_pipebase_vfptr *vfptr, struct grid_epbase *epbase)
{
    grid_assert (epbase->ep->sock);

    grid_fsm_init (&self->fsm, NULL, NULL, 0, self, &epbase->ep->sock->fsm);
    self->vfptr = vfptr;
    self->state = GRID_PIPEBASE_STATE_IDLE;
    self->instate = GRID_PIPEBASE_INSTATE_DEACTIVATED;
    self->outstate = GRID_PIPEBASE_OUTSTATE_DEACTIVATED;
    self->sock = epbase->ep->sock;
    memcpy (&self->options, &epbase->ep->options,
        sizeof (struct grid_ep_options));
    grid_fsm_event_init (&self->in);
    grid_fsm_event_init (&self->out);
}

void grid_pipebase_term (struct grid_pipebase *self)
{
    grid_assert_state (self, GRID_PIPEBASE_STATE_IDLE);

    grid_fsm_event_term (&self->out);
    grid_fsm_event_term (&self->in);
    grid_fsm_term (&self->fsm);
}

int grid_pipebase_start (struct grid_pipebase *self)
{
    int rc;

    grid_assert_state (self, GRID_PIPEBASE_STATE_IDLE);

    self->state = GRID_PIPEBASE_STATE_ACTIVE;
    self->instate = GRID_PIPEBASE_INSTATE_ASYNC;
    self->outstate = GRID_PIPEBASE_OUTSTATE_IDLE;
    rc = grid_sock_add (self->sock, (struct grid_pipe*) self);
    if (grid_slow (rc < 0)) {
        self->state = GRID_PIPEBASE_STATE_FAILED;
        return rc;
    }
    if (self->sock)
        grid_fsm_raise (&self->fsm, &self->out, GRID_PIPE_OUT);

    return 0;
}

void grid_pipebase_stop (struct grid_pipebase *self)
{
    if (self->state == GRID_PIPEBASE_STATE_ACTIVE)
        grid_sock_rm (self->sock, (struct grid_pipe*) self);
    self->state = GRID_PIPEBASE_STATE_IDLE;
}

void grid_pipebase_received (struct grid_pipebase *self)
{
    if (grid_fast (self->instate == GRID_PIPEBASE_INSTATE_RECEIVING)) {
        self->instate = GRID_PIPEBASE_INSTATE_RECEIVED;
        return;
    }
    grid_assert (self->instate == GRID_PIPEBASE_INSTATE_ASYNC);
    self->instate = GRID_PIPEBASE_INSTATE_IDLE;
    if (self->sock)
        grid_fsm_raise (&self->fsm, &self->in, GRID_PIPE_IN);
}

void grid_pipebase_sent (struct grid_pipebase *self)
{
    if (grid_fast (self->outstate == GRID_PIPEBASE_OUTSTATE_SENDING)) {
        self->outstate = GRID_PIPEBASE_OUTSTATE_SENT;
        return;
    }
    grid_assert (self->outstate == GRID_PIPEBASE_OUTSTATE_ASYNC);
    self->outstate = GRID_PIPEBASE_OUTSTATE_IDLE;
    if (self->sock)
        grid_fsm_raise (&self->fsm, &self->out, GRID_PIPE_OUT);
}

void grid_pipebase_getopt (struct grid_pipebase *self, int level, int option,
    void *optval, size_t *optvallen)
{
    int rc;
    int intval;

    if (level == GRID_SOL_SOCKET) {
        switch (option) {

        /*  Endpoint options  */
        case GRID_SNDPRIO:
            intval = self->options.sndprio;
            break;
        case GRID_RCVPRIO:
            intval = self->options.rcvprio;
            break;
        case GRID_IPV4ONLY:
            intval = self->options.ipv4only;
            break;

        /*  Fallback to socket options  */
        default:
            rc = grid_sock_getopt_inner (self->sock, level,
                option, optval, optvallen);
            errnum_assert (rc == 0, -rc);
            return;
        }

        memcpy (optval, &intval,
            *optvallen < sizeof (int) ? *optvallen : sizeof (int));
        *optvallen = sizeof (int);

        return;
    }

    rc = grid_sock_getopt_inner (self->sock, level, option, optval, optvallen);
    errnum_assert (rc == 0, -rc);
}

int grid_pipebase_ispeer (struct grid_pipebase *self, int socktype)
{
    return grid_sock_ispeer (self->sock, socktype);
}

void grid_pipe_setdata (struct grid_pipe *self, void *data)
{
    ((struct grid_pipebase*) self)->data = data;
}

void *grid_pipe_getdata (struct grid_pipe *self)
{
    return ((struct grid_pipebase*) self)->data;
}

int grid_pipe_send (struct grid_pipe *self, struct grid_msg *msg)
{
    int rc;
    struct grid_pipebase *pipebase;

    pipebase = (struct grid_pipebase*) self;
    grid_assert (pipebase->outstate == GRID_PIPEBASE_OUTSTATE_IDLE);
    pipebase->outstate = GRID_PIPEBASE_OUTSTATE_SENDING;
    rc = pipebase->vfptr->send (pipebase, msg);
    errnum_assert (rc >= 0, -rc);
    if (grid_fast (pipebase->outstate == GRID_PIPEBASE_OUTSTATE_SENT)) {
        pipebase->outstate = GRID_PIPEBASE_OUTSTATE_IDLE;
        return rc;
    }
    grid_assert (pipebase->outstate == GRID_PIPEBASE_OUTSTATE_SENDING);
    pipebase->outstate = GRID_PIPEBASE_OUTSTATE_ASYNC;
    return rc | GRID_PIPEBASE_RELEASE;
}

int grid_pipe_recv (struct grid_pipe *self, struct grid_msg *msg)
{
    int rc;
    struct grid_pipebase *pipebase;

    pipebase = (struct grid_pipebase*) self;
    grid_assert (pipebase->instate == GRID_PIPEBASE_INSTATE_IDLE);
    pipebase->instate = GRID_PIPEBASE_INSTATE_RECEIVING;
    rc = pipebase->vfptr->recv (pipebase, msg);
    errnum_assert (rc >= 0, -rc);

    if (grid_fast (pipebase->instate == GRID_PIPEBASE_INSTATE_RECEIVED)) {
        pipebase->instate = GRID_PIPEBASE_INSTATE_IDLE;
        return rc;
    }
    grid_assert (pipebase->instate == GRID_PIPEBASE_INSTATE_RECEIVING);
    pipebase->instate = GRID_PIPEBASE_INSTATE_ASYNC;
    return rc | GRID_PIPEBASE_RELEASE;
}

void grid_pipe_getopt (struct grid_pipe *self, int level, int option,
    void *optval, size_t *optvallen)
{

    struct grid_pipebase *pipebase;

    pipebase = (struct grid_pipebase*) self;
    grid_pipebase_getopt (pipebase, level, option, optval, optvallen);
}


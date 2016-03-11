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

#include "req.h"
#include "xreq.h"

#include "../../grid.h"
#include "../../reqrep.h"

#include "../../aio/fsm.h"
#include "../../aio/timer.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/random.h"
#include "../../utils/wire.h"
#include "../../utils/list.h"
#include "../../utils/int.h"
#include "../../utils/attr.h"

#include <stddef.h>
#include <string.h>

/*  Default re-send interval is 1 minute. */
#define GRID_REQ_DEFAULT_RESEND_IVL 60000

#define GRID_REQ_STATE_IDLE 1
#define GRID_REQ_STATE_PASSIVE 2
#define GRID_REQ_STATE_DELAYED 3
#define GRID_REQ_STATE_ACTIVE 4
#define GRID_REQ_STATE_TIMED_OUT 5
#define GRID_REQ_STATE_CANCELLING 6
#define GRID_REQ_STATE_STOPPING_TIMER 7
#define GRID_REQ_STATE_DONE 8
#define GRID_REQ_STATE_STOPPING 9

#define GRID_REQ_ACTION_START 1
#define GRID_REQ_ACTION_IN 2
#define GRID_REQ_ACTION_OUT 3
#define GRID_REQ_ACTION_SENT 4
#define GRID_REQ_ACTION_RECEIVED 5
#define GRID_REQ_ACTION_PIPE_RM 6

#define GRID_REQ_SRC_RESEND_TIMER 1

static const struct grid_sockbase_vfptr grid_req_sockbase_vfptr = {
    grid_req_stop,
    grid_req_destroy,
    grid_xreq_add,
    grid_req_rm,
    grid_req_in,
    grid_req_out,
    grid_req_events,
    grid_req_csend,
    grid_req_crecv,
    grid_req_setopt,
    grid_req_getopt
};

void grid_req_init (struct grid_req *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint)
{
    grid_req_handle hndl;

    grid_xreq_init (&self->xreq, vfptr, hint);
    grid_fsm_init_root (&self->fsm, grid_req_handler, grid_req_shutdown,
        grid_sockbase_getctx (&self->xreq.sockbase));
    self->state = GRID_REQ_STATE_IDLE;

    /*  Start assigning request IDs beginning with a random number. This way
        there should be no key clashes even if the executable is re-started. */
    grid_random_generate (&self->lastid, sizeof (self->lastid));

    self->task.sent_to = NULL;

    grid_msg_init (&self->task.request, 0);
    grid_msg_init (&self->task.reply, 0);
    grid_timer_init (&self->task.timer, GRID_REQ_SRC_RESEND_TIMER, &self->fsm);
    self->resend_ivl = GRID_REQ_DEFAULT_RESEND_IVL;

    /*  For now, handle is empty. */
    memset (&hndl, 0, sizeof (hndl));
    grid_task_init (&self->task, self->lastid, hndl);

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);
}

void grid_req_term (struct grid_req *self)
{
    grid_timer_term (&self->task.timer);
    grid_task_term (&self->task);
    grid_msg_term (&self->task.reply);
    grid_msg_term (&self->task.request);
    grid_fsm_term (&self->fsm);
    grid_xreq_term (&self->xreq);
}

void grid_req_stop (struct grid_sockbase *self)
{
    struct grid_req *req;

    req = grid_cont (self, struct grid_req, xreq.sockbase);

    grid_fsm_stop (&req->fsm);
}

void grid_req_destroy (struct grid_sockbase *self)
{
    struct grid_req *req;

    req = grid_cont (self, struct grid_req, xreq.sockbase);

    grid_req_term (req);
    grid_free (req);
}

int grid_req_inprogress (struct grid_req *self)
{
    /*  Return 1 if there's a request submitted. 0 otherwise. */
    return self->state == GRID_REQ_STATE_IDLE ||
        self->state == GRID_REQ_STATE_PASSIVE ||
        self->state == GRID_REQ_STATE_STOPPING ? 0 : 1;
}

void grid_req_in (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    int rc;
    struct grid_req *req;
    uint32_t reqid;

    req = grid_cont (self, struct grid_req, xreq.sockbase);

    /*  Pass the pipe to the raw REQ socket. */
    grid_xreq_in (&req->xreq.sockbase, pipe);

    while (1) {

        /*  Get new reply. */
        rc = grid_xreq_recv (&req->xreq.sockbase, &req->task.reply);
        if (grid_slow (rc == -EAGAIN))
            return;
        errnum_assert (rc == 0, -rc);

        /*  No request was sent. Getting a reply doesn't make sense. */
        if (grid_slow (!grid_req_inprogress (req))) {
            grid_msg_term (&req->task.reply);
            continue;
        }

        /*  Ignore malformed replies. */
        if (grid_slow (grid_chunkref_size (&req->task.reply.sphdr) !=
              sizeof (uint32_t))) {
            grid_msg_term (&req->task.reply);
            continue;
        }

        /*  Ignore replies with incorrect request IDs. */
        reqid = grid_getl (grid_chunkref_data (&req->task.reply.sphdr));
        if (grid_slow (!(reqid & 0x80000000))) {
            grid_msg_term (&req->task.reply);
            continue;
        }
        if (grid_slow (reqid != (req->task.id | 0x80000000))) {
            grid_msg_term (&req->task.reply);
            continue;
        }

        /*  Trim the request ID. */
        grid_chunkref_term (&req->task.reply.sphdr);
        grid_chunkref_init (&req->task.reply.sphdr, 0);

        /*  TODO: Deallocate the request here? */

        /*  Notify the state machine. */
        if (req->state == GRID_REQ_STATE_ACTIVE)
            grid_fsm_action (&req->fsm, GRID_REQ_ACTION_IN);

        return;
    }
}

void grid_req_out (struct grid_sockbase *self, struct grid_pipe *pipe)
{
    struct grid_req *req;

    req = grid_cont (self, struct grid_req, xreq.sockbase);

    /*  Add the pipe to the underlying raw socket. */
    grid_xreq_out (&req->xreq.sockbase, pipe);

    /*  Notify the state machine. */
    if (req->state == GRID_REQ_STATE_DELAYED)
        grid_fsm_action (&req->fsm, GRID_REQ_ACTION_OUT);
}

int grid_req_events (struct grid_sockbase *self)
{
    int rc;
    struct grid_req *req;

    req = grid_cont (self, struct grid_req, xreq.sockbase);

    /*  OUT is signalled all the time because sending a request while
        another one is being processed cancels the old one. */
    rc = GRID_SOCKBASE_EVENT_OUT;

    /*  In DONE state the reply is stored in 'reply' field. */
    if (req->state == GRID_REQ_STATE_DONE)
        rc |= GRID_SOCKBASE_EVENT_IN;

    return rc;
}

int grid_req_send (GRID_UNUSED int s, GRID_UNUSED grid_req_handle hndl,
    GRID_UNUSED const void *buf, GRID_UNUSED size_t len, GRID_UNUSED int flags)
{
    grid_assert (0);
}

int grid_req_csend (struct grid_sockbase *self, struct grid_msg *msg)
{
    struct grid_req *req;

    req = grid_cont (self, struct grid_req, xreq.sockbase);

    /*  Generate new request ID for the new request and put it into message
        header. The most important bit is set to 1 to indicate that this is
        the bottom of the backtrace stack. */
    ++req->task.id;
    grid_assert (grid_chunkref_size (&msg->sphdr) == 0);
    grid_chunkref_term (&msg->sphdr);
    grid_chunkref_init (&msg->sphdr, 4);
    grid_putl (grid_chunkref_data (&msg->sphdr), req->task.id | 0x80000000);

    /*  Store the message so that it can be re-sent if there's no reply. */
    grid_msg_term (&req->task.request);
    grid_msg_mv (&req->task.request, msg);

    /*  Notify the state machine. */
    grid_fsm_action (&req->fsm, GRID_REQ_ACTION_SENT);

    return 0;
}

int grid_req_recv (GRID_UNUSED int s, GRID_UNUSED grid_req_handle *hndl,
    GRID_UNUSED void *buf, GRID_UNUSED size_t len, GRID_UNUSED int flags)
{
    grid_assert (0);
}


int grid_req_crecv (struct grid_sockbase *self, struct grid_msg *msg)
{
    struct grid_req *req;

    req = grid_cont (self, struct grid_req, xreq.sockbase);

    /*  No request was sent. Waiting for a reply doesn't make sense. */
    if (grid_slow (!grid_req_inprogress (req)))
        return -EFSM;

    /*  If reply was not yet recieved, wait further. */
    if (grid_slow (req->state != GRID_REQ_STATE_DONE))
        return -EAGAIN;

    /*  If the reply was already received, just pass it to the caller. */
    grid_msg_mv (msg, &req->task.reply);
    grid_msg_init (&req->task.reply, 0);

    /*  Notify the state machine. */
    grid_fsm_action (&req->fsm, GRID_REQ_ACTION_RECEIVED);

    return 0;
}

int grid_req_setopt (struct grid_sockbase *self, int level, int option,
        const void *optval, size_t optvallen)
{
    struct grid_req *req;

    req = grid_cont (self, struct grid_req, xreq.sockbase);

    if (level != GRID_REQ)
        return -ENOPROTOOPT;

    if (option == GRID_REQ_RESEND_IVL) {
        if (grid_slow (optvallen != sizeof (int)))
            return -EINVAL;
        req->resend_ivl = *(int*) optval;
        return 0;
    }

    return -ENOPROTOOPT;
}

int grid_req_getopt (struct grid_sockbase *self, int level, int option,
        void *optval, size_t *optvallen)
{
    struct grid_req *req;

    req = grid_cont (self, struct grid_req, xreq.sockbase);

    if (level != GRID_REQ)
        return -ENOPROTOOPT;

    if (option == GRID_REQ_RESEND_IVL) {
        if (grid_slow (*optvallen < sizeof (int)))
            return -EINVAL;
        *(int*) optval = req->resend_ivl;
        *optvallen = sizeof (int);
        return 0;
    }

    return -ENOPROTOOPT;
}

void grid_req_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_req *req;

    req = grid_cont (self, struct grid_req, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        grid_timer_stop (&req->task.timer);
        req->state = GRID_REQ_STATE_STOPPING;
    }
    if (grid_slow (req->state == GRID_REQ_STATE_STOPPING)) {
        if (!grid_timer_isidle (&req->task.timer))
            return;
        req->state = GRID_REQ_STATE_IDLE;
        grid_fsm_stopped_noevent (&req->fsm);
        grid_sockbase_stopped (&req->xreq.sockbase);
        return;
    }

    grid_fsm_bad_state(req->state, src, type);
}

void grid_req_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_req *req;

    req = grid_cont (self, struct grid_req, fsm);

    switch (req->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  The socket was created recently. Intermediate state.                      */
/*  Pass straight to the PASSIVE state.                                       */
/******************************************************************************/
    case GRID_REQ_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                req->state = GRID_REQ_STATE_PASSIVE;
                return;
            default:
                grid_fsm_bad_action (req->state, src, type);
            }

        default:
            grid_fsm_bad_source (req->state, src, type);
        }

/******************************************************************************/
/*  PASSIVE state.                                                            */
/*  No request is submitted.                                                  */
/******************************************************************************/
    case GRID_REQ_STATE_PASSIVE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_REQ_ACTION_SENT:
                grid_req_action_send (req, 1);
                return;
            default:
                grid_fsm_bad_action (req->state, src, type);
            }

        default:
            grid_fsm_bad_source (req->state, src, type);
        }

/******************************************************************************/
/*  DELAYED state.                                                            */
/*  Request was submitted but it could not be sent to the network because     */
/*  there was no peer available at the moment. Now we are waiting for the     */
/*  peer to arrive to send the request to it.                                 */
/******************************************************************************/
    case GRID_REQ_STATE_DELAYED:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_REQ_ACTION_OUT:
                grid_req_action_send (req, 0);
                return;
            case GRID_REQ_ACTION_SENT:
                return;
            default:
                grid_fsm_bad_action (req->state, src, type);
            }

        default:
            grid_fsm_bad_source (req->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  Request was submitted. Waiting for reply.                                 */
/******************************************************************************/
    case GRID_REQ_STATE_ACTIVE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_REQ_ACTION_IN:

                /*  Reply arrived. */
                grid_timer_stop (&req->task.timer);
                req->task.sent_to = NULL;
                req->state = GRID_REQ_STATE_STOPPING_TIMER;
                return;

            case GRID_REQ_ACTION_SENT:

                /*  New request was sent while the old one was still being
                    processed. Cancel the old request first. */
                grid_timer_stop (&req->task.timer);
                req->task.sent_to = NULL;
                req->state = GRID_REQ_STATE_CANCELLING;
                return;

            case GRID_REQ_ACTION_PIPE_RM:
                /*  Pipe that we sent request to is removed  */
                grid_timer_stop (&req->task.timer);
                req->task.sent_to = NULL;
                /*  Pretend we timed out so request resent immediately  */
                req->state = GRID_REQ_STATE_TIMED_OUT;
                return;

            default:
                grid_fsm_bad_action (req->state, src, type);
            }

        case GRID_REQ_SRC_RESEND_TIMER:
            switch (type) {
            case GRID_TIMER_TIMEOUT:
                grid_timer_stop (&req->task.timer);
                req->task.sent_to = NULL;
                req->state = GRID_REQ_STATE_TIMED_OUT;
                return;
            default:
                grid_fsm_bad_action (req->state, src, type);
            }

        default:
            grid_fsm_bad_source (req->state, src, type);
        }

/******************************************************************************/
/*  TIMED_OUT state.                                                          */
/*  Waiting for reply has timed out. Stopping the timer. Afterwards, we'll    */
/*  re-send the request.                                                      */
/******************************************************************************/
    case GRID_REQ_STATE_TIMED_OUT:
        switch (src) {

        case GRID_REQ_SRC_RESEND_TIMER:
            switch (type) {
            case GRID_TIMER_STOPPED:
                grid_req_action_send (req, 1);
                return;
            default:
                grid_fsm_bad_action (req->state, src, type);
            }

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_REQ_ACTION_SENT:
                req->state = GRID_REQ_STATE_CANCELLING;
                return;
            default:
                grid_fsm_bad_action (req->state, src, type);
            }

        default:
            grid_fsm_bad_source (req->state, src, type);
        }

/******************************************************************************/
/*  CANCELLING state.                                                         */
/*  Request was canceled. Waiting till the timer is stopped. Note that        */
/*  cancelling is done by sending a new request. Thus there's already         */
/*  a request waiting to be sent in this state.                               */
/******************************************************************************/
    case GRID_REQ_STATE_CANCELLING:
        switch (src) {

        case GRID_REQ_SRC_RESEND_TIMER:
            switch (type) {
            case GRID_TIMER_STOPPED:

                /*  Timer is stopped. Now we can send the delayed request. */
                grid_req_action_send (req, 1);
                return;

            default:
                grid_fsm_bad_action (req->state, src, type);
            }

        case GRID_FSM_ACTION:
             switch (type) {
             case GRID_REQ_ACTION_SENT:

                 /*  No need to do anything here. Old delayed request is just
                     replaced by the new one that will be sent once the timer
                     is closed. */
                 return;

             default:
                 grid_fsm_bad_action (req->state, src, type);
             }

        default:
            grid_fsm_bad_source (req->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_TIMER state.                                                     */
/*  Reply was delivered. Waiting till the timer is stopped.                   */
/******************************************************************************/
    case GRID_REQ_STATE_STOPPING_TIMER:
        switch (src) {

        case GRID_REQ_SRC_RESEND_TIMER:

            switch (type) {
            case GRID_TIMER_STOPPED:
                req->state = GRID_REQ_STATE_DONE;
                return;
            default:
                grid_fsm_bad_action (req->state, src, type);
            }

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_REQ_ACTION_SENT:
                req->state = GRID_REQ_STATE_CANCELLING;
                return;
            default:
                grid_fsm_bad_action (req->state, src, type);
            }

        default:
            grid_fsm_bad_source (req->state, src, type);
        }

/******************************************************************************/
/*  DONE state.                                                               */
/*  Reply was received but not yet retrieved by the user.                     */
/******************************************************************************/
    case GRID_REQ_STATE_DONE:
        switch (src) {

        case GRID_FSM_ACTION:
             switch (type) {
             case GRID_REQ_ACTION_RECEIVED:
                 req->state = GRID_REQ_STATE_PASSIVE;
                 return;
             case GRID_REQ_ACTION_SENT:
                 grid_req_action_send (req, 1);
                 return;
             default:
                 grid_fsm_bad_action (req->state, src, type);
             }

        default:
            grid_fsm_bad_source (req->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (req->state, src, type);
    }
}

/******************************************************************************/
/*  State machine actions.                                                    */
/******************************************************************************/

void grid_req_action_send (struct grid_req *self, int allow_delay)
{
    int rc;
    struct grid_msg msg;
    struct grid_pipe *to;

    /*  Send the request. */
    grid_msg_cp (&msg, &self->task.request);
    rc = grid_xreq_send_to (&self->xreq.sockbase, &msg, &to);

    /*  If the request cannot be sent at the moment wait till
        new outbound pipe arrives. */
    if (grid_slow (rc == -EAGAIN)) {
        grid_assert (allow_delay == 1);
        grid_msg_term (&msg);
        self->state = GRID_REQ_STATE_DELAYED;
        return;
    }

    /*  Request was successfully sent. Set up the re-send timer
        in case the request gets lost somewhere further out
        in the topology. */
    if (grid_fast (rc == 0)) {
        grid_timer_start (&self->task.timer, self->resend_ivl);
        grid_assert (to);
        self->task.sent_to = to;
        self->state = GRID_REQ_STATE_ACTIVE;
        return;
    }

    /*  Unexpected error. */
    errnum_assert (0, -rc);
}

static int grid_req_create (void *hint, struct grid_sockbase **sockbase)
{
    struct grid_req *self;

    self = grid_alloc (sizeof (struct grid_req), "socket (req)");
    alloc_assert (self);
    grid_req_init (self, &grid_req_sockbase_vfptr, hint);
    *sockbase = &self->xreq.sockbase;

    return 0;
}

void grid_req_rm (struct grid_sockbase *self, struct grid_pipe *pipe) {
    struct grid_req *req;

    req = grid_cont (self, struct grid_req, xreq.sockbase);

    grid_xreq_rm (self, pipe);
    if (grid_slow (pipe == req->task.sent_to)) {
        grid_fsm_action (&req->fsm, GRID_REQ_ACTION_PIPE_RM);
    }
}

static struct grid_socktype grid_req_socktype_struct = {
    AF_SP,
    GRID_REQ,
    0,
    grid_req_create,
    grid_xreq_ispeer,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_socktype *grid_req_socktype = &grid_req_socktype_struct;


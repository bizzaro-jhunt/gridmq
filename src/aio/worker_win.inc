/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.

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

#include "ctx.h"
#include "usock.h"

#include "../utils/err.h"
#include "../utils/cont.h"
#include "../utils/fast.h"

#define GRID_WORKER_MAX_EVENTS 32

#define GRID_WORKER_OP_STATE_IDLE 1
#define GRID_WORKER_OP_STATE_ACTIVE 2
#define GRID_WORKER_OP_STATE_ACTIVE_ZEROISERROR 3

/*  The value of this variable is irrelevant. It's used only as a placeholder
    for the address that is used as the 'stop' event ID. */
const int grid_worker_stop = 0;

/*  Private functions. */
static void grid_worker_routine (void *arg);

void grid_worker_task_init (struct grid_worker_task *self, int src,
    struct grid_fsm *owner)
{
    self->src = src;
    self->owner = owner;
}

void grid_worker_task_term (struct grid_worker_task *self)
{
}

void grid_worker_op_init (struct grid_worker_op *self, int src,
    struct grid_fsm *owner)
{
    self->src = src;
    self->owner = owner;
    self->state = GRID_WORKER_OP_STATE_IDLE;
}

void grid_worker_op_term (struct grid_worker_op *self)
{
    grid_assert_state (self, GRID_WORKER_OP_STATE_IDLE);
}

void grid_worker_op_start (struct grid_worker_op *self, int zeroiserror)
{
    grid_assert_state (self, GRID_WORKER_OP_STATE_IDLE);
    self->state = zeroiserror ? GRID_WORKER_OP_STATE_ACTIVE_ZEROISERROR :
        GRID_WORKER_OP_STATE_ACTIVE;
}

int grid_worker_op_isidle (struct grid_worker_op *self)
{
    return self->state == GRID_WORKER_OP_STATE_IDLE ? 1 : 0;
}

int grid_worker_init (struct grid_worker *self)
{
    self->cp = CreateIoCompletionPort (INVALID_HANDLE_VALUE, NULL, 0, 0);
    win_assert (self->cp);
    grid_timerset_init (&self->timerset);
    grid_thread_init (&self->thread, grid_worker_routine, self);

    return 0;
}

void grid_worker_term (struct grid_worker *self)
{
    BOOL brc;

    /*  Ask worker thread to terminate. */
    brc = PostQueuedCompletionStatus (self->cp, 0,
        (ULONG_PTR) &grid_worker_stop, NULL);
    win_assert (brc);

    /*  Wait till worker thread terminates. */
    grid_thread_term (&self->thread);

    grid_timerset_term (&self->timerset);
    brc = CloseHandle (self->cp);
    win_assert (brc);
}

void grid_worker_execute (struct grid_worker *self, struct grid_worker_task *task)
{
    BOOL brc;

    brc = PostQueuedCompletionStatus (self->cp, 0, (ULONG_PTR) task, NULL);
    win_assert (brc);
}

void grid_worker_add_timer (struct grid_worker *self, int timeout,
    struct grid_worker_timer *timer)
{
    grid_timerset_add (&((struct grid_worker*) self)->timerset, timeout,
        &timer->hndl);
}

void grid_worker_rm_timer (struct grid_worker *self, struct grid_worker_timer *timer)
{
    grid_timerset_rm (&((struct grid_worker*) self)->timerset, &timer->hndl);
}

HANDLE grid_worker_getcp (struct grid_worker *self)
{
    return self->cp;
}

static void grid_worker_routine (void *arg)
{
    int rc;
    BOOL brc;
    struct grid_worker *self;
    int timeout;
    ULONG count;
    ULONG i;
    struct grid_timerset_hndl *thndl;
    struct grid_worker_timer *timer;
    struct grid_worker_task *task;
    struct grid_worker_op *op;
    OVERLAPPED_ENTRY entries [GRID_WORKER_MAX_EVENTS];

    self = (struct grid_worker*) arg;

    while (1) {

        /*  Process all expired timers. */
        while (1) {
            rc = grid_timerset_event (&self->timerset, &thndl);
            if (grid_fast (rc == -EAGAIN))
                break;
            errnum_assert (rc == 0, -rc);
            timer = grid_cont (thndl, struct grid_worker_timer, hndl);
            grid_ctx_enter (timer->owner->ctx);
            grid_fsm_feed (timer->owner, -1, GRID_WORKER_TIMER_TIMEOUT, timer);
            grid_ctx_leave (timer->owner->ctx);
        }

        /*  Compute the time interval till next timer expiration. */
        timeout = grid_timerset_timeout (&self->timerset);

        /*  Wait for new events and/or timeouts. */
        brc = GetQueuedCompletionStatusEx (self->cp, entries,
            GRID_WORKER_MAX_EVENTS, &count, timeout < 0 ? INFINITE : timeout,
            FALSE);
        if (grid_slow (!brc && GetLastError () == WAIT_TIMEOUT))
            continue;
        win_assert (brc);

        for (i = 0; i != count; ++i) {

            /*  Process I/O completion events. */
            if (grid_fast (entries [i].lpOverlapped)) {
                op = grid_cont (entries [i].lpOverlapped,
                    struct grid_worker_op, olpd);

                /*  The 'Internal' field is actually an NTSTATUS. Report
                    success and error. Ignore warnings and informational
                    messages.*/
                rc = entries [i].Internal & 0xc0000000;
                switch (rc) {
                case 0x00000000:
                     rc = GRID_WORKER_OP_DONE;
                     break;
                case 0xc0000000:
                     rc = GRID_WORKER_OP_ERROR;
                     break;
                default:
                     continue;
                }

                /*  Raise the completion event. */
                grid_ctx_enter (op->owner->ctx);
                grid_assert (op->state != GRID_WORKER_OP_STATE_IDLE);
                if (rc != GRID_WORKER_OP_ERROR &&
                      op->state == GRID_WORKER_OP_STATE_ACTIVE_ZEROISERROR &&
                      entries [i].dwNumberOfBytesTransferred == 0)
                    rc = GRID_WORKER_OP_ERROR;
                op->state = GRID_WORKER_OP_STATE_IDLE;
                grid_fsm_feed (op->owner, op->src, rc, op);
                grid_ctx_leave (op->owner->ctx);

                continue;
            }

            /*  Worker thread shutdown is requested. */
            if (grid_slow (entries [i].lpCompletionKey ==
                  (ULONG_PTR) &grid_worker_stop))
                return;

            /*  Process tasks. */
            task = (struct grid_worker_task*) entries [i].lpCompletionKey;
            grid_ctx_enter (task->owner->ctx);
            grid_fsm_feed (task->owner, task->src,
                GRID_WORKER_TASK_EXECUTE, task);
            grid_ctx_leave (task->owner->ctx);
        }
    }
}

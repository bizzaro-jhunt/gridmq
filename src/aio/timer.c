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

#include "timer.h"

#include "../utils/cont.h"
#include "../utils/fast.h"
#include "../utils/err.h"
#include "../utils/attr.h"

/*  Timer state reflects the state as seen by the user thread. It says nothing
    about the state of affairs in the worker thread. */
#define GRID_TIMER_STATE_IDLE 1
#define GRID_TIMER_STATE_ACTIVE 2
#define GRID_TIMER_STATE_STOPPING 3

#define GRID_TIMER_SRC_START_TASK 1
#define GRID_TIMER_SRC_STOP_TASK 2

/*  Private functions. */
static void grid_timer_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_timer_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);

void grid_timer_init (struct grid_timer *self, int src, struct grid_fsm *owner)
{
    grid_fsm_init (&self->fsm, grid_timer_handler, grid_timer_shutdown,
        src, self, owner);
    self->state = GRID_TIMER_STATE_IDLE;
    grid_worker_task_init (&self->start_task, GRID_TIMER_SRC_START_TASK,
        &self->fsm);
    grid_worker_task_init (&self->stop_task, GRID_TIMER_SRC_STOP_TASK, &self->fsm);
    grid_worker_timer_init (&self->wtimer, &self->fsm);
    grid_fsm_event_init (&self->done);
    self->worker = grid_fsm_choose_worker (&self->fsm);
    self->timeout = -1;
}

void grid_timer_term (struct grid_timer *self)
{
    grid_assert_state (self, GRID_TIMER_STATE_IDLE);

    grid_fsm_event_term (&self->done);
    grid_worker_timer_term (&self->wtimer);
    grid_worker_task_term (&self->stop_task);
    grid_worker_task_term (&self->start_task);
    grid_fsm_term (&self->fsm);
}

int grid_timer_isidle (struct grid_timer *self)
{
     return grid_fsm_isidle (&self->fsm);
}

void grid_timer_start (struct grid_timer *self, int timeout)
{
    /*  Negative timeout make no sense. */
    grid_assert (timeout >= 0);

    self->timeout = timeout;
    grid_fsm_start (&self->fsm);
}

void grid_timer_stop (struct grid_timer *self)
{
    grid_fsm_stop (&self->fsm);
}

static void grid_timer_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_timer *timer;

    timer = grid_cont (self, struct grid_timer, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        timer->state = GRID_TIMER_STATE_STOPPING;
        grid_worker_execute (timer->worker, &timer->stop_task);
        return;
    }
    if (grid_slow (timer->state == GRID_TIMER_STATE_STOPPING)) {
        if (src != GRID_TIMER_SRC_STOP_TASK)
            return;
        grid_assert (type == GRID_WORKER_TASK_EXECUTE);
        grid_worker_rm_timer (timer->worker, &timer->wtimer);
        timer->state = GRID_TIMER_STATE_IDLE;
        grid_fsm_stopped (&timer->fsm, GRID_TIMER_STOPPED);
        return;
    }

    grid_fsm_bad_state(timer->state, src, type);
}

static void grid_timer_handler (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    struct grid_timer *timer;

    timer = grid_cont (self, struct grid_timer, fsm);

    switch (timer->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case GRID_TIMER_STATE_IDLE:
        switch (src) {
        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:

                /*  Send start event to the worker thread. */
                timer->state = GRID_TIMER_STATE_ACTIVE;
                grid_worker_execute (timer->worker, &timer->start_task);
                return;
            default:
                grid_fsm_bad_action (timer->state, src, type);
            }
        default:
            grid_fsm_bad_source (timer->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case GRID_TIMER_STATE_ACTIVE:
        if (src == GRID_TIMER_SRC_START_TASK) {
            grid_assert (type == GRID_WORKER_TASK_EXECUTE);
            grid_assert (timer->timeout >= 0);
            grid_worker_add_timer (timer->worker, timer->timeout,
                &timer->wtimer);
            timer->timeout = -1;
            return;
        }
        if (srcptr == &timer->wtimer) {
            switch (type) {
            case GRID_WORKER_TIMER_TIMEOUT:

                /*  Notify the user about the timeout. */
                grid_assert (timer->timeout == -1);
                grid_fsm_raise (&timer->fsm, &timer->done, GRID_TIMER_TIMEOUT);
                return;

            default:
                grid_fsm_bad_action (timer->state, src, type);
            }
        }
        grid_fsm_bad_source (timer->state, src, type);

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (timer->state, src, type);
    }
}


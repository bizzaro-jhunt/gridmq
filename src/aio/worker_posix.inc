/*
    Copyright (c) 2013-2014 Martin Sustrik  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.
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

#include "ctx.h"

#include "../utils/err.h"
#include "../utils/fast.h"
#include "../utils/cont.h"
#include "../utils/attr.h"
#include "../utils/queue.h"

/*  Private functions. */
static void grid_worker_routine (void *arg);

void grid_worker_fd_init (struct grid_worker_fd *self, int src,
    struct grid_fsm *owner)
{
    self->src = src;
    self->owner = owner;
}

void grid_worker_fd_term (GRID_UNUSED struct grid_worker_fd *self)
{
}

void grid_worker_add_fd (struct grid_worker *self, int s, struct grid_worker_fd *fd)
{
    grid_poller_add (&((struct grid_worker*) self)->poller, s, &fd->hndl);
}

void grid_worker_rm_fd (struct grid_worker *self, struct grid_worker_fd *fd)
{
    grid_poller_rm (&((struct grid_worker*) self)->poller, &fd->hndl);
}

void grid_worker_set_in (struct grid_worker *self, struct grid_worker_fd *fd)
{
    grid_poller_set_in (&((struct grid_worker*) self)->poller, &fd->hndl);
}

void grid_worker_reset_in (struct grid_worker *self, struct grid_worker_fd *fd)
{
    grid_poller_reset_in (&((struct grid_worker*) self)->poller, &fd->hndl);
}

void grid_worker_set_out (struct grid_worker *self, struct grid_worker_fd *fd)
{
    grid_poller_set_out (&((struct grid_worker*) self)->poller, &fd->hndl);
}

void grid_worker_reset_out (struct grid_worker *self, struct grid_worker_fd *fd)
{
    grid_poller_reset_out (&((struct grid_worker*) self)->poller, &fd->hndl);
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

void grid_worker_task_init (struct grid_worker_task *self, int src,
    struct grid_fsm *owner)
{
    self->src = src;
    self->owner = owner;
    grid_queue_item_init (&self->item);
}

void grid_worker_task_term (struct grid_worker_task *self)
{
    grid_queue_item_term (&self->item);
}

int grid_worker_init (struct grid_worker *self)
{
    int rc;

    rc = grid_efd_init (&self->efd);
    if (rc < 0)
        return rc;

    grid_mutex_init (&self->sync);
    grid_queue_init (&self->tasks);
    grid_queue_item_init (&self->stop);
    grid_poller_init (&self->poller);
    grid_poller_add (&self->poller, grid_efd_getfd (&self->efd), &self->efd_hndl);
    grid_poller_set_in (&self->poller, &self->efd_hndl);
    grid_timerset_init (&self->timerset);
    grid_thread_init (&self->thread, grid_worker_routine, self);

    return 0;
}

void grid_worker_term (struct grid_worker *self)
{
    /*  Ask worker thread to terminate. */
    grid_mutex_lock (&self->sync);
    grid_queue_push (&self->tasks, &self->stop);
    grid_efd_signal (&self->efd);
    grid_mutex_unlock (&self->sync);

    /*  Wait till worker thread terminates. */
    grid_thread_term (&self->thread);

    /*  Clean up. */
    grid_timerset_term (&self->timerset);
    grid_poller_term (&self->poller);
    grid_efd_term (&self->efd);
    grid_queue_item_term (&self->stop);
    grid_queue_term (&self->tasks);
    grid_mutex_term (&self->sync);
}

void grid_worker_execute (struct grid_worker *self, struct grid_worker_task *task)
{
    grid_mutex_lock (&self->sync);
    grid_queue_push (&self->tasks, &task->item);
    grid_efd_signal (&self->efd);
    grid_mutex_unlock (&self->sync);
}

void grid_worker_cancel (struct grid_worker *self, struct grid_worker_task *task)
{
    grid_mutex_lock (&self->sync);
    grid_queue_remove (&self->tasks, &task->item);
    grid_mutex_unlock (&self->sync);
}

static void grid_worker_routine (void *arg)
{
    int rc;
    struct grid_worker *self;
    int pevent;
    struct grid_poller_hndl *phndl;
    struct grid_timerset_hndl *thndl;
    struct grid_queue tasks;
    struct grid_queue_item *item;
    struct grid_worker_task *task;
    struct grid_worker_fd *fd;
    struct grid_worker_timer *timer;

    self = (struct grid_worker*) arg;

    /*  Infinite loop. It will be interrupted only when the object is
        shut down. */
    while (1) {

        /*  Wait for new events and/or timeouts. */
        rc = grid_poller_wait (&self->poller,
            grid_timerset_timeout (&self->timerset));
        errnum_assert (rc == 0, -rc);

        /*  Process all expired timers. */
        while (1) {
            rc = grid_timerset_event (&self->timerset, &thndl);
            if (rc == -EAGAIN)
                break;
            errnum_assert (rc == 0, -rc);
            timer = grid_cont (thndl, struct grid_worker_timer, hndl);
            grid_ctx_enter (timer->owner->ctx);
            grid_fsm_feed (timer->owner, -1, GRID_WORKER_TIMER_TIMEOUT, timer);
            grid_ctx_leave (timer->owner->ctx);
        }

        /*  Process all events from the poller. */
        while (1) {

            /*  Get next poller event, such as IN or OUT. */
            rc = grid_poller_event (&self->poller, &pevent, &phndl);
            if (grid_slow (rc == -EAGAIN))
                break;

            /*  If there are any new incoming worker tasks, process them. */
            if (phndl == &self->efd_hndl) {
                grid_assert (pevent == GRID_POLLER_IN);

                /*  Make a local copy of the task queue. This way
                    the application threads are not blocked and can post new
                    tasks while the existing tasks are being processed. Also,
                    new tasks can be posted from within task handlers. */
                grid_mutex_lock (&self->sync);
                grid_efd_unsignal (&self->efd);
                memcpy (&tasks, &self->tasks, sizeof (tasks));
                grid_queue_init (&self->tasks);
                grid_mutex_unlock (&self->sync);

                while (1) {

                    /*  Next worker task. */
                    item = grid_queue_pop (&tasks);
                    if (grid_slow (!item))
                        break;

                    /*  If the worker thread is asked to stop, do so. */
                    if (grid_slow (item == &self->stop)) {
			/*  Make sure we remove all the other workers from
			    the queue, because we're not doing anything with
			    them. */
			while (grid_queue_pop (&tasks) != NULL) {
				continue;
			}
                        grid_queue_term (&tasks);
                        return;
                    }

                    /*  It's a user-defined task. Notify the user that it has
                        arrived in the worker thread. */
                    task = grid_cont (item, struct grid_worker_task, item);
                    grid_ctx_enter (task->owner->ctx);
                    grid_fsm_feed (task->owner, task->src,
                        GRID_WORKER_TASK_EXECUTE, task);
                    grid_ctx_leave (task->owner->ctx);
                }
                grid_queue_term (&tasks);
                continue;
            }

            /*  It's a true I/O event. Invoke the handler. */
            fd = grid_cont (phndl, struct grid_worker_fd, hndl);
            grid_ctx_enter (fd->owner->ctx);
            grid_fsm_feed (fd->owner, fd->src, pevent, fd);
            grid_ctx_leave (fd->owner->ctx);
        }
    }
}


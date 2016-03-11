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

#ifndef GRID_WORKER_INCLUDED
#define GRID_WORKER_INCLUDED

#include "fsm.h"
#include "timerset.h"
#include "../utils/queue.h"
#include "../utils/mutex.h"
#include "../utils/thread.h"
#include "../utils/efd.h"

#include "poller.h"

#define GRID_WORKER_FD_IN GRID_POLLER_IN
#define GRID_WORKER_FD_OUT GRID_POLLER_OUT
#define GRID_WORKER_FD_ERR GRID_POLLER_ERR

struct grid_worker_fd {
    int src;
    struct grid_fsm *owner;
    struct grid_poller_hndl hndl;
};

void grid_worker_fd_init (struct grid_worker_fd *self, int src,
    struct grid_fsm *owner);
void grid_worker_fd_term (struct grid_worker_fd *self);

struct grid_worker_task {
    int src;
    struct grid_fsm *owner;
    struct grid_queue_item item;
};

struct grid_worker {
    struct grid_mutex sync;
    struct grid_queue tasks;
    struct grid_queue_item stop;
    struct grid_efd efd;
    struct grid_poller poller;
    struct grid_poller_hndl efd_hndl;
    struct grid_timerset timerset;
    struct grid_thread thread;
};

void grid_worker_add_fd (struct grid_worker *self, int s, struct grid_worker_fd *fd);
void grid_worker_rm_fd(struct grid_worker *self, struct grid_worker_fd *fd);
void grid_worker_set_in (struct grid_worker *self, struct grid_worker_fd *fd);
void grid_worker_reset_in (struct grid_worker *self, struct grid_worker_fd *fd);
void grid_worker_set_out (struct grid_worker *self, struct grid_worker_fd *fd);
void grid_worker_reset_out (struct grid_worker *self, struct grid_worker_fd *fd);

#define GRID_WORKER_TIMER_TIMEOUT 1

struct grid_worker_timer {
    struct grid_fsm *owner;
    struct grid_timerset_hndl hndl;
};

void grid_worker_timer_init (struct grid_worker_timer *self,
    struct grid_fsm *owner);
void grid_worker_timer_term (struct grid_worker_timer *self);
int grid_worker_timer_isactive (struct grid_worker_timer *self);

#define GRID_WORKER_TASK_EXECUTE 1

struct grid_worker_task;

void grid_worker_task_init (struct grid_worker_task *self, int src,
    struct grid_fsm *owner);
void grid_worker_task_term (struct grid_worker_task *self);

struct grid_worker;

int grid_worker_init (struct grid_worker *self);
void grid_worker_term (struct grid_worker *self);
void grid_worker_execute (struct grid_worker *self, struct grid_worker_task *task);
void grid_worker_cancel (struct grid_worker *self, struct grid_worker_task *task);

void grid_worker_add_timer (struct grid_worker *self, int timeout,
    struct grid_worker_timer *timer);
void grid_worker_rm_timer (struct grid_worker *self,
    struct grid_worker_timer *timer);

#endif


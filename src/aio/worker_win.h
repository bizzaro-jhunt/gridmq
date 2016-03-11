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

#include "fsm.h"
#include "timerset.h"

#include "../utils/win.h"
#include "../utils/thread.h"

struct grid_worker_task {
    int src;
    struct grid_fsm *owner;
};

#define GRID_WORKER_OP_DONE 1
#define GRID_WORKER_OP_ERROR 2

struct grid_worker_op {
    int src;
    struct grid_fsm *owner;
    int state;

    /*  This structure is to be used by the user, not grid_worker_op itself.
        Actual usage is specific to the asynchronous operation in question. */
    OVERLAPPED olpd;
};

void grid_worker_op_init (struct grid_worker_op *self, int src,
    struct grid_fsm *owner);
void grid_worker_op_term (struct grid_worker_op *self);

/*  Call this function when asynchronous operation is started.
    If 'zeroiserror' is set to 1, zero bytes transferred will be treated
    as an error. */
void grid_worker_op_start (struct grid_worker_op *self, int zeroiserror);

int grid_worker_op_isidle (struct grid_worker_op *self);

struct grid_worker {
    HANDLE cp;
    struct grid_timerset timerset;
    struct grid_thread thread;
};

HANDLE grid_worker_getcp (struct grid_worker *self);

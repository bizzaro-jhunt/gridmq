/*
    Copyright (c) 2014 Martin Sustrik  All rights reserved.

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

#ifndef GRID_TASK_INCLUDED
#define GRID_TASK_INCLUDED

#include "../../reqrep.h"

#include "../../aio/fsm.h"
#include "../../aio/timer.h"
#include "../../utils/msg.h"
#include "../../utils/int.h"

struct grid_task {

    /*  ID of the request being currently processed. Replies for different
        requests are considered stale and simply dropped. */
    uint32_t id;

    /*  User-defined handle of the task. */
    grid_req_handle hndl;

    /*  Stored request, so that it can be re-sent if needed. */
    struct grid_msg request;

    /*  Stored reply, so that user can retrieve it later on. */
    struct grid_msg reply;

    /*  Timer used to wait while request should be re-sent. */
    struct grid_timer timer;

    /*  Pipe the current request has been sent to. This is an optimisation so
        that request can be re-sent immediately if the pipe disappears.  */
    struct grid_pipe *sent_to;
};

void grid_task_init (struct grid_task *self, uint32_t id, grid_req_handle hndl);
void grid_task_term (struct grid_task *self);

#endif


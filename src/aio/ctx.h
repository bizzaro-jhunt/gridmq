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

#ifndef GRID_CTX_INCLUDED
#define GRID_CTX_INCLUDED

#include "../utils/mutex.h"
#include "../utils/queue.h"

#include "worker.h"
#include "pool.h"
#include "fsm.h"

/*  AIO context for objects using AIO subsystem. */

typedef void (*grid_ctx_onleave) (struct grid_ctx *self);

struct grid_ctx {
    struct grid_mutex sync;
    struct grid_pool *pool;
    struct grid_queue events;
    struct grid_queue eventsto;
    grid_ctx_onleave onleave;
};

void grid_ctx_init (struct grid_ctx *self, struct grid_pool *pool,
    grid_ctx_onleave onleave);
void grid_ctx_term (struct grid_ctx *self);

void grid_ctx_enter (struct grid_ctx *self);
void grid_ctx_leave (struct grid_ctx *self);

struct grid_worker *grid_ctx_choose_worker (struct grid_ctx *self);

void grid_ctx_raise (struct grid_ctx *self, struct grid_fsm_event *event);
void grid_ctx_raiseto (struct grid_ctx *self, struct grid_fsm_event *event);

#endif


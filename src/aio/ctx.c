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

#include "ctx.h"

#include "../utils/err.h"
#include "../utils/cont.h"
#include "../utils/fast.h"

void grid_ctx_init (struct grid_ctx *self, struct grid_pool *pool,
    grid_ctx_onleave onleave)
{
    grid_mutex_init (&self->sync);
    self->pool = pool;
    grid_queue_init (&self->events);
    grid_queue_init (&self->eventsto);
    self->onleave = onleave;
}

void grid_ctx_term (struct grid_ctx *self)
{
    grid_queue_term (&self->eventsto);
    grid_queue_term (&self->events);
    grid_mutex_term (&self->sync);
}

void grid_ctx_enter (struct grid_ctx *self)
{
    grid_mutex_lock (&self->sync);
}

void grid_ctx_leave (struct grid_ctx *self)
{
    struct grid_queue_item *item;
    struct grid_fsm_event *event;
    struct grid_queue eventsto;

    /*  Process any queued events before leaving the context. */
    while (1) {
        item = grid_queue_pop (&self->events);
        event = grid_cont (item, struct grid_fsm_event, item);
        if (!event)
            break;
        grid_fsm_event_process (event);
    }

    /*  Notify the owner that we are leaving the context. */
    if (grid_fast (self->onleave != NULL))
        self->onleave (self);

    /*  Shortcut in the case there are no external events. */
    if (grid_queue_empty (&self->eventsto)) {
        grid_mutex_unlock (&self->sync);
        return;
    }

    /*  Make a copy of the queue of the external events so that it does not
        get corrupted once we unlock the context. */
    eventsto = self->eventsto;
    grid_queue_init (&self->eventsto);

    grid_mutex_unlock (&self->sync);

    /*  Process any queued external events. Before processing each event
        lock the context it belongs to. */
    while (1) {
        item = grid_queue_pop (&eventsto);
        event = grid_cont (item, struct grid_fsm_event, item);
        if (!event)
            break;
        grid_ctx_enter (event->fsm->ctx);
        grid_fsm_event_process (event);
        grid_ctx_leave (event->fsm->ctx);
    }

    grid_queue_term (&eventsto);
}

struct grid_worker *grid_ctx_choose_worker (struct grid_ctx *self)
{
    return grid_pool_choose_worker (self->pool);
}

void grid_ctx_raise (struct grid_ctx *self, struct grid_fsm_event *event)
{
    grid_queue_push (&self->events, &event->item);
}

void grid_ctx_raiseto (struct grid_ctx *self, struct grid_fsm_event *event)
{
    grid_queue_push (&self->eventsto, &event->item);
}


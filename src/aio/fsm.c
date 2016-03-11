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
#include "ctx.h"

#include "../utils/err.h"

#include <stddef.h>

#define GRID_FSM_STATE_IDLE 1
#define GRID_FSM_STATE_ACTIVE 2
#define GRID_FSM_STATE_STOPPING 3

void grid_fsm_event_init (struct grid_fsm_event *self)
{
    self->fsm = NULL;
    self->src = -1;
    self->srcptr = NULL;
    self->type = -1;
    grid_queue_item_init (&self->item);
}

void grid_fsm_event_term (struct grid_fsm_event *self)
{
    grid_queue_item_term (&self->item);
}

int grid_fsm_event_active (struct grid_fsm_event *self)
{
    return grid_queue_item_isinqueue (&self->item);
}

void grid_fsm_event_process (struct grid_fsm_event *self)
{
    int src;
    int type;
    void *srcptr;

    src = self->src;
    type = self->type;
    srcptr = self->srcptr;
    self->src = -1;
    self->type = -1;
    self->srcptr = NULL;

    grid_fsm_feed (self->fsm, src, type, srcptr);
}

void grid_fsm_feed (struct grid_fsm *self, int src, int type, void *srcptr)
{
    if (grid_slow (self->state != GRID_FSM_STATE_STOPPING)) {
        self->fn (self, src, type, srcptr);
    } else {
        self->shutdown_fn (self, src, type, srcptr);
    }
}

void grid_fsm_init_root (struct grid_fsm *self, grid_fsm_fn fn,
    grid_fsm_fn shutdown_fn, struct grid_ctx *ctx)
{
    self->fn = fn;
    self->shutdown_fn = shutdown_fn;
    self->state = GRID_FSM_STATE_IDLE;
    self->src = -1;
    self->srcptr = NULL;
    self->owner = NULL;
    self->ctx = ctx;
    grid_fsm_event_init (&self->stopped);
}

void grid_fsm_init (struct grid_fsm *self, grid_fsm_fn fn,
    grid_fsm_fn shutdown_fn, int src, void *srcptr, struct grid_fsm *owner)
{
    self->fn = fn;
    self->shutdown_fn = shutdown_fn;
    self->state = GRID_FSM_STATE_IDLE;
    self->src = src;
    self->srcptr = srcptr;
    self->owner = owner;
    self->ctx = owner->ctx;
    grid_fsm_event_init (&self->stopped);
}

void grid_fsm_term (struct grid_fsm *self)
{
    grid_assert (grid_fsm_isidle (self));
    grid_fsm_event_term (&self->stopped);
}

void grid_fsm_start (struct grid_fsm *self)
{
    grid_assert (grid_fsm_isidle (self));
    self->fn (self, GRID_FSM_ACTION, GRID_FSM_START, NULL);
    self->state = GRID_FSM_STATE_ACTIVE;
}

int grid_fsm_isidle (struct grid_fsm *self)
{
    return self->state == GRID_FSM_STATE_IDLE &&
        !grid_fsm_event_active (&self->stopped) ? 1 : 0;
}

void grid_fsm_stop (struct grid_fsm *self)
{
    /*  If stopping of the state machine was already requested, do nothing. */
    if (self->state != GRID_FSM_STATE_ACTIVE)
        return;

    self->state = GRID_FSM_STATE_STOPPING;
    self->shutdown_fn (self, GRID_FSM_ACTION, GRID_FSM_STOP, NULL);
}

void grid_fsm_stopped (struct grid_fsm *self, int type)
{
    grid_assert_state (self, GRID_FSM_STATE_STOPPING);
    grid_fsm_raise (self, &self->stopped, type);
    self->state = GRID_FSM_STATE_IDLE;
}

void grid_fsm_stopped_noevent (struct grid_fsm *self)
{
    grid_assert_state (self, GRID_FSM_STATE_STOPPING);
    self->state = GRID_FSM_STATE_IDLE;
}

void grid_fsm_swap_owner (struct grid_fsm *self, struct grid_fsm_owner *owner)
{
    int oldsrc;
    struct grid_fsm *oldowner;

    oldsrc = self->src;
    oldowner = self->owner;
    self->src = owner->src;
    self->owner = owner->fsm;
    owner->src = oldsrc;
    owner->fsm = oldowner;
}

struct grid_worker *grid_fsm_choose_worker (struct grid_fsm *self)
{
    return grid_ctx_choose_worker (self->ctx);
}

void grid_fsm_action (struct grid_fsm *self, int type)
{
    grid_assert (type > 0);
    grid_fsm_feed (self, GRID_FSM_ACTION, type, NULL);
}

void grid_fsm_raise (struct grid_fsm *self, struct grid_fsm_event *event, int type)
{
    event->fsm = self->owner;
    event->src = self->src;
    event->srcptr = self->srcptr;
    event->type = type;
    grid_ctx_raise (self->ctx, event);
}

void grid_fsm_raiseto (struct grid_fsm *self, struct grid_fsm *dst,
    struct grid_fsm_event *event, int src, int type, void *srcptr)
{
    event->fsm = dst;
    event->src = src;
    event->srcptr = srcptr;
    event->type = type;
    grid_ctx_raiseto (self->ctx, event);
}


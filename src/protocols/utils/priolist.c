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

#include "priolist.h"

#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/err.h"
#include "../../utils/attr.h"

#include <stddef.h>

void grid_priolist_init (struct grid_priolist *self)
{
    int i;

    for (i = 0; i != GRID_PRIOLIST_SLOTS; ++i) {
        grid_list_init (&self->slots [i].pipes);
        self->slots [i].current = NULL;
    }
    self->current = -1;
}

void grid_priolist_term (struct grid_priolist *self)
{
    int i;

    for (i = 0; i != GRID_PRIOLIST_SLOTS; ++i)
        grid_list_term (&self->slots [i].pipes);
}

void grid_priolist_add (GRID_UNUSED struct grid_priolist *self,
    struct grid_priolist_data *data, struct grid_pipe *pipe, int priority)
{
    data->pipe = pipe;
    data->priority = priority;
    grid_list_item_init (&data->item);
}

void grid_priolist_rm (struct grid_priolist *self, struct grid_priolist_data *data)
{
    struct grid_priolist_slot *slot;
    struct grid_list_item *it;

    /*  Non-active pipes don't need any special processing. */
    if (!grid_list_item_isinlist (&data->item)) {
        grid_list_item_term (&data->item);
        return;
    }

    /*  If the pipe being removed is not current, we can simply erase it
        from the list. */
    slot = &self->slots [data->priority - 1];
    if (slot->current != data) {
        grid_list_erase (&slot->pipes, &data->item);
        grid_list_item_term (&data->item);
        return;
    }

    /*  Advance the current pointer (with wrap-over). */
    it = grid_list_erase (&slot->pipes, &data->item);
    slot->current = grid_cont (it, struct grid_priolist_data, item);
    grid_list_item_term (&data->item);
    if (!slot->current) {
        it = grid_list_begin (&slot->pipes);
        slot->current = grid_cont (it, struct grid_priolist_data, item);
    }

    /*  If we are not messing with the current slot, we are done. */
    if (self->current != data->priority)
        return;

    /*  Otherwise, the current slot may have become empty and we have switch
        to lower priority slots. */
    while (grid_list_empty (&self->slots [self->current - 1].pipes)) {
        ++self->current;
        if (self->current > GRID_PRIOLIST_SLOTS) {
            self->current = -1;
            return;
        }
    }
}

void grid_priolist_activate (struct grid_priolist *self,
    struct grid_priolist_data *data)
{
    struct grid_priolist_slot *slot;

    slot = &self->slots [data->priority - 1];

    /*  If there are already some elements in this slot, current pipe is not
        going to change. */
    if (!grid_list_empty (&slot->pipes)) {
        grid_list_insert (&slot->pipes, &data->item, grid_list_end (&slot->pipes));
        return;
    }

    /*  Add first pipe into the slot. If there are no pipes in priolist at all
        this slot becomes current. */
    grid_list_insert (&slot->pipes, &data->item, grid_list_end (&slot->pipes));
    slot->current = data;
    if (self->current == -1) {
        self->current = data->priority;
        return;
    }

    /*  If the current priority is lower than the one of the newly activated
        pipe, this slot becomes current. */
    if (self->current > data->priority) {
        self->current = data->priority;
        return;
    }

    /*  Current doesn't change otherwise. */
}

int grid_priolist_is_active (struct grid_priolist *self)
{
    return self->current == -1 ? 0 : 1;
}

struct grid_pipe *grid_priolist_getpipe (struct grid_priolist *self)
{
    if (grid_slow (self->current == -1))
        return NULL;
    return self->slots [self->current - 1].current->pipe;
}

void grid_priolist_advance (struct grid_priolist *self, int release)
{
    struct grid_priolist_slot *slot;
    struct grid_list_item *it;

    grid_assert (self->current > 0);
    slot = &self->slots [self->current - 1];

    /*  Move slot's current pointer to the next pipe. */
    if (release)
        it = grid_list_erase (&slot->pipes, &slot->current->item);
    else
        it = grid_list_next (&slot->pipes, &slot->current->item);
    if (!it)
        it = grid_list_begin (&slot->pipes);
    slot->current = grid_cont (it, struct grid_priolist_data, item);

    /* If there are no more pipes in this slot, find a non-empty slot with
       lower priority. */
    while (grid_list_empty (&slot->pipes)) {
        ++self->current;
        if (self->current > GRID_PRIOLIST_SLOTS) {
            self->current = -1;
            return;
        }
        slot = &self->slots [self->current - 1];
    }
}

int grid_priolist_get_priority (struct grid_priolist *self) {
    return self->current;
}

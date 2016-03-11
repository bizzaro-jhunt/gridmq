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

#ifndef GRID_PRIOLIST_INCLUDED
#define GRID_PRIOLIST_INCLUDED

#include "../../protocol.h"

#include "../../utils/list.h"

/*  Prioritised list of pipes. */

#define GRID_PRIOLIST_SLOTS 16

struct grid_priolist_data {

    /*  The underlying pipe itself. */
    struct grid_pipe *pipe;

    /*  Priority the pipe is assigned. Using this value we can find the
        grid_priolist_slot object that owns this pipe. */
    int priority;

    /*  The structure is a member in grid_priolist_slot's 'pipes' list. */
    struct grid_list_item item;
};

struct grid_priolist_slot {

    /*  The list of pipes on particular priority level. */
    struct grid_list pipes;

    /*  Pointer to the current pipe within the priority level. If there's no
        pipe available, the field is set to NULL. */
    struct grid_priolist_data *current;
};

struct grid_priolist {

    /*  Each slot holds pipes for a particular priority level. */
    struct grid_priolist_slot slots [GRID_PRIOLIST_SLOTS];

    /*  The index of the slot holding the current pipe. It should be the
        highest-priority non-empty slot available. If there's no available
        pipe, this field is set to -1. */
    int current;
};

/*  Initialise the list. */
void grid_priolist_init (struct grid_priolist *self);

/*  Terminate the list. The list must be empty before it's terminated. */
void grid_priolist_term (struct grid_priolist *self);

/*  Add a new pipe to the list with a particular priority level. The pipe
    is not active at this point. Use grid_priolist_activate to activate it. */
void grid_priolist_add (struct grid_priolist *self, struct grid_priolist_data *data,
    struct grid_pipe *pipe, int priority);

/*  Remove the pipe from the list. */
void grid_priolist_rm (struct grid_priolist *self, struct grid_priolist_data *data);

/*  Activates a non-active pipe. The pipe must be added to the list prior to
    calling this function. */
void grid_priolist_activate (struct grid_priolist *self, struct grid_priolist_data *data);

/*  Returns 1 if there's at least a single active pipe in the list,
    0 otherwise. */
int grid_priolist_is_active (struct grid_priolist *self);

/*  Get the pointer to the current pipe. If there's no pipe in the list,
    NULL is returned. */
struct grid_pipe *grid_priolist_getpipe (struct grid_priolist *self);

/*  Moves to the next pipe in the list. If 'release' is set to 1, the current
    pipe is removed from the list. To re-insert it into the list use
    grid_priolist_activate function. */
void grid_priolist_advance (struct grid_priolist *self, int release);

/*  Returns current priority. Used for statistics only  */
int grid_priolist_get_priority (struct grid_priolist *self);

#endif

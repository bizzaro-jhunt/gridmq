/*
    Copyright (c) 2012-2014 Martin Sustrik  All rights reserved.

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

#ifndef GRID_QUEUE_INCLUDED
#define GRID_QUEUE_INCLUDED

/*  Undefined value for initialising a queue item which is not
    part of a queue. */
#define GRID_QUEUE_NOTINQUEUE ((struct grid_queue_item*) -1)

/*  Use for initialising a queue item statically. */
#define GRID_QUEUE_ITEM_INITIALIZER {GRID_LIST_NOTINQUEUE}

struct grid_queue_item {
    struct grid_queue_item *next;
};

struct grid_queue {
    struct grid_queue_item *head;
    struct grid_queue_item *tail;
};

/*  Initialise the queue. */
void grid_queue_init (struct grid_queue *self);

/*  Terminate the queue. Note that queue must be manually emptied before the
    termination. */
void grid_queue_term (struct grid_queue *self);

/*  Returns 1 if there are no items in the queue, 0 otherwise. */
int grid_queue_empty (struct grid_queue *self);

/*  Inserts one element into the queue. */
void grid_queue_push (struct grid_queue *self, struct grid_queue_item *item);

/*  Remove the item if it is present in the queue. */
void grid_queue_remove (struct grid_queue *self, struct grid_queue_item *item);

/*  Retrieves one element from the queue. The element is removed
    from the queue. Returns NULL if the queue is empty. */
struct grid_queue_item *grid_queue_pop (struct grid_queue *self);

/*  Initialise a queue item. At this point it is not a part of any queue. */
void grid_queue_item_init (struct grid_queue_item *self);

/*  Terminate a queue item. The item must not be in a queue prior to
    this call. */
void grid_queue_item_term (struct grid_queue_item *self);

/*  Returns 1 if item is a part of a queue. 0 otherwise. */
int grid_queue_item_isinqueue (struct grid_queue_item *self);

#endif

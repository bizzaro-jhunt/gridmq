/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.

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

#ifndef GRID_LIST_INCLUDED
#define GRID_LIST_INCLUDED

struct grid_list_item {
    struct grid_list_item *next;
    struct grid_list_item *prev;
};

struct grid_list {
    struct grid_list_item *first;
    struct grid_list_item *last;
};

/*  Undefined value for initializing a list item which is not part of a list. */
#define GRID_LIST_NOTINLIST ((struct grid_list_item*) -1)

/*  Use for initializing a list item statically. */
#define GRID_LIST_ITEM_INITIALIZER {GRID_LIST_NOTINLIST, GRID_LIST_NOTINLIST}

/*  Initialise the list. */
void grid_list_init (struct grid_list *self);

/*  Terminates the list. Note that all items must be removed before the
    termination. */
void grid_list_term (struct grid_list *self);

/*  Returns 1 is list has zero items, 0 otherwise. */
int grid_list_empty (struct grid_list *self);

/*  Returns iterator to the first item in the list. */
struct grid_list_item *grid_list_begin (struct grid_list *self);

/*  Returns iterator to one past the last item in the list. */
struct grid_list_item *grid_list_end (struct grid_list *self);

/*  Returns iterator to an item prior to the one pointed to by 'it'. */
struct grid_list_item *grid_list_prev (struct grid_list *self,
    struct grid_list_item *it);

/*  Returns iterator to one past the item pointed to by 'it'. */
struct grid_list_item *grid_list_next (struct grid_list *self,
    struct grid_list_item *it);

/*  Adds the item to the list before the item pointed to by 'it'. Priot to
    insertion item should not be part of any list. */
void grid_list_insert (struct grid_list *self, struct grid_list_item *item,
    struct grid_list_item *it);

/*  Removes the item from the list and returns pointer to the next item in the
    list. Item must be part of the list. */
struct grid_list_item *grid_list_erase (struct grid_list *self,
    struct grid_list_item *item);

/*  Initialize a list item. At this point it is not part of any list. */
void grid_list_item_init (struct grid_list_item *self);

/*  Terminates a list item. Item must not be part of any list before it's
    terminated. */
void grid_list_item_term (struct grid_list_item *self);

/*  Returns 1 is the item is part of a list, 0 otherwise. */
int grid_list_item_isinlist (struct grid_list_item *self);

#endif

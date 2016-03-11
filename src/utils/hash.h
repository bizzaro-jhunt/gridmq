/*
    Copyright (c) 2012-2013 Martin Sustrik  All rights reserved.

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

#ifndef GRID_HASH_INCLUDED
#define GRID_HASH_INCLUDED

#include "list.h"
#include "int.h"

#include <stddef.h>

/*  Use for initialising a hash item statically. */
#define GRID_HASH_ITEM_INITIALIZER {0xffff, GRID_LIST_ITEM_INITILIZER}

struct grid_hash_item {
    uint32_t key;
    struct grid_list_item list;
};

struct grid_hash {
    uint32_t slots;
    uint32_t items;
    struct grid_list *array;
};

/*  Initialise the hash table. */
void grid_hash_init (struct grid_hash *self);

/*  Terminate the hash. Note that hash must be manually emptied before the
    termination. */
void grid_hash_term (struct grid_hash *self);

/*  Adds an item to the hash. */
void grid_hash_insert (struct grid_hash *self, uint32_t key,
    struct grid_hash_item *item);

/*  Removes the element from the hash it is in at the moment. */
void grid_hash_erase (struct grid_hash *self, struct grid_hash_item *item);

/*  Gets an item in the hash based on the key. Returns NULL if there's no
    corresponing item in the hash table. */
struct grid_hash_item *grid_hash_get (struct grid_hash *self, uint32_t key);

/*  Initialise a hash item. At this point it is not a part of any hash table. */
void grid_hash_item_init (struct grid_hash_item *self);

/*  Terminate a hash item. The item must not be in a hash table prior to
    this call. */
void grid_hash_item_term (struct grid_hash_item *self);

#endif

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

#include "hash.h"
#include "fast.h"
#include "alloc.h"
#include "cont.h"
#include "err.h"

#define GRID_HASH_INITIAL_SLOTS 32

static uint32_t grid_hash_key (uint32_t key);

void grid_hash_init (struct grid_hash *self)
{
    uint32_t i;

    self->slots = GRID_HASH_INITIAL_SLOTS;
    self->items = 0;
    self->array = grid_alloc (sizeof (struct grid_list) * GRID_HASH_INITIAL_SLOTS,
        "hash map");
    alloc_assert (self->array);
    for (i = 0; i != GRID_HASH_INITIAL_SLOTS; ++i)
        grid_list_init (&self->array [i]);
}

void grid_hash_term (struct grid_hash *self)
{
    uint32_t i;

    for (i = 0; i != self->slots; ++i)
        grid_list_term (&self->array [i]);
    grid_free (self->array);
}

static void grid_hash_rehash (struct grid_hash *self) {
    uint32_t i;
    uint32_t oldslots;
    struct grid_list *oldarray;
    struct grid_hash_item *hitm;
    uint32_t newslot;

    /*  Allocate new double-sized array of slots. */
    oldslots = self->slots;
    oldarray = self->array;
    self->slots *= 2;
    self->array = grid_alloc (sizeof (struct grid_list) * self->slots, "hash map");
    alloc_assert (self->array);
    for (i = 0; i != self->slots; ++i)
    grid_list_init (&self->array [i]);

    /*  Move the items from old slot array to new slot array. */
    for (i = 0; i != oldslots; ++i) {
    while (!grid_list_empty (&oldarray [i])) {
        hitm = grid_cont (grid_list_begin (&oldarray [i]),
                struct grid_hash_item, list);
        grid_list_erase (&oldarray [i], &hitm->list);
        newslot = grid_hash_key (hitm->key) % self->slots;
        grid_list_insert (&self->array [newslot], &hitm->list,
                grid_list_end (&self->array [newslot]));
    }

    grid_list_term (&oldarray [i]);
    }

    /*  Deallocate the old array of slots. */
    grid_free (oldarray);
}


void grid_hash_insert (struct grid_hash *self, uint32_t key,
    struct grid_hash_item *item)
{
    struct grid_list_item *it;
    uint32_t i;

    i = grid_hash_key (key) % self->slots;

    for (it = grid_list_begin (&self->array [i]);
          it != grid_list_end (&self->array [i]);
          it = grid_list_next (&self->array [i], it))
        grid_assert (grid_cont (it, struct grid_hash_item, list)->key != key);

    item->key = key;
    grid_list_insert (&self->array [i], &item->list,
        grid_list_end (&self->array [i]));
    ++self->items;

    /*  If the hash is getting full, double the amount of slots and
        re-hash all the items. */
    if (grid_slow (self->items * 2 > self->slots && self->slots < 0x80000000))
    grid_hash_rehash(self);
}

void grid_hash_erase (struct grid_hash *self, struct grid_hash_item *item)
{
    uint32_t slot;

    slot = grid_hash_key (item->key) % self->slots;
    grid_list_erase (&self->array [slot], &item->list);
	--self->items;
}

struct grid_hash_item *grid_hash_get (struct grid_hash *self, uint32_t key)
{
    uint32_t slot;
    struct grid_list_item *it;
    struct grid_hash_item *item;

    slot = grid_hash_key (key) % self->slots;

    for (it = grid_list_begin (&self->array [slot]);
          it != grid_list_end (&self->array [slot]);
          it = grid_list_next (&self->array [slot], it)) {
        item = grid_cont (it, struct grid_hash_item, list);
        if (item->key == key)
            return item;
    }

    return NULL;
}

uint32_t grid_hash_key (uint32_t key)
{
    /*  TODO: This is a randomly chosen hashing function. Give some thought
        to picking a more fitting one. */
    key = (key ^ 61) ^ (key >> 16);
    key += key << 3;
    key = key ^ (key >> 4);
    key = key * 0x27d4eb2d;
    key = key ^ (key >> 15);

    return key;
}

void grid_hash_item_init (struct grid_hash_item *self)
{
    grid_list_item_init (&self->list);
}

void grid_hash_item_term (struct grid_hash_item *self)
{
    grid_list_item_term (&self->list);
}


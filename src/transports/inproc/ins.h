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

#ifndef GRID_INS_INCLUDED
#define GRID_INS_INCLUDED

#include "../../transport.h"

#include "../../utils/list.h"

/*  Inproc naming system. A global repository of inproc endpoints. */

struct grid_ins_item {

    /*  Every ins_item is an endpoint. */
    struct grid_epbase epbase;

    /*  Every ins_item is either in the list of bound or connected endpoints. */
    struct grid_list_item item;

    /*  This is the local cache of the endpoint's protocol ID. This way we can
        check the value without actually locking the object. */
    int protocol;
};

void grid_ins_item_init (struct grid_ins_item *self,
    const struct grid_epbase_vfptr *vfptr, void *hint);
void grid_ins_item_term (struct grid_ins_item *self);

void grid_ins_init (void);
void grid_ins_term (void);

typedef void (*grid_ins_fn) (struct grid_ins_item *self, struct grid_ins_item *peer);

int grid_ins_bind (struct grid_ins_item *item, grid_ins_fn fn);
void grid_ins_connect (struct grid_ins_item *item, grid_ins_fn fn);
void grid_ins_disconnect (struct grid_ins_item *item);
void grid_ins_unbind (struct grid_ins_item *item);

#endif


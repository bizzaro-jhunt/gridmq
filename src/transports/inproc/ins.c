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

#include "ins.h"

#include "../../utils/mutex.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/err.h"

struct grid_ins {

    /*  Synchronises access to this object. */
    struct grid_mutex sync;

    /*  List of all bound inproc endpoints. */
    /*  TODO: O(n) lookup, shouldn't we do better? Hash? */
    struct grid_list bound;

    /*  List of all connected inproc endpoints. */
    /*  TODO: O(n) lookup, shouldn't we do better? Hash? */
    struct grid_list connected;
};

/*  Global instance of the grid_ins object. It contains the lists of all
    inproc endpoints in the current process. */
static struct grid_ins self;

void grid_ins_item_init (struct grid_ins_item *self,
    const struct grid_epbase_vfptr *vfptr, void *hint)
{
    size_t sz;

    grid_epbase_init (&self->epbase, vfptr, hint);
    grid_list_item_init (&self->item);
    sz = sizeof (self->protocol);
    grid_epbase_getopt (&self->epbase, GRID_SOL_SOCKET, GRID_PROTOCOL,
        &self->protocol, &sz);
    grid_assert (sz == sizeof (self->protocol));
}

void grid_ins_item_term (struct grid_ins_item *self)
{
    grid_list_item_term (&self->item);
    grid_epbase_term (&self->epbase);
}

void grid_ins_init (void)
{
    grid_mutex_init (&self.sync);
    grid_list_init (&self.bound);
    grid_list_init (&self.connected);
}
void grid_ins_term (void)
{
    grid_list_term (&self.connected);
    grid_list_term (&self.bound);
    grid_mutex_term (&self.sync);
}

int grid_ins_bind (struct grid_ins_item *item, grid_ins_fn fn)
{
    struct grid_list_item *it;
    struct grid_ins_item *bitem;
    struct grid_ins_item *citem;

    grid_mutex_lock (&self.sync);

    /*  Check whether the endpoint isn't already bound. */
    /*  TODO:  This is an O(n) algorithm! */
    for (it = grid_list_begin (&self.bound); it != grid_list_end (&self.bound);
          it = grid_list_next (&self.bound, it)) {
        bitem = grid_cont (it, struct grid_ins_item, item);
        if (strncmp (grid_epbase_getaddr (&item->epbase),
              grid_epbase_getaddr (&bitem->epbase), GRID_SOCKADDR_MAX) == 0) {
            grid_mutex_unlock (&self.sync);
            return -EADDRINUSE;
        }
    }

    /*  Insert the entry into the endpoint repository. */
    grid_list_insert (&self.bound, &item->item,
        grid_list_end (&self.bound));

    /*  During this process new pipes may be created. */
    for (it = grid_list_begin (&self.connected);
          it != grid_list_end (&self.connected);
          it = grid_list_next (&self.connected, it)) {
        citem = grid_cont (it, struct grid_ins_item, item);
        if (strncmp (grid_epbase_getaddr (&item->epbase),
              grid_epbase_getaddr (&citem->epbase), GRID_SOCKADDR_MAX) == 0) {

            /*  Check whether the two sockets are compatible. */
            if (!grid_epbase_ispeer (&item->epbase, citem->protocol))
                continue;

            fn (item, citem);
        }
    }

    grid_mutex_unlock (&self.sync);

    return 0;
}

void grid_ins_connect (struct grid_ins_item *item, grid_ins_fn fn)
{
    struct grid_list_item *it;
    struct grid_ins_item *bitem;

    grid_mutex_lock (&self.sync);

    /*  Insert the entry into the endpoint repository. */
    grid_list_insert (&self.connected, &item->item,
        grid_list_end (&self.connected));

    /*  During this process a pipe may be created. */
    for (it = grid_list_begin (&self.bound);
          it != grid_list_end (&self.bound);
          it = grid_list_next (&self.bound, it)) {
        bitem = grid_cont (it, struct grid_ins_item, item);
        if (strncmp (grid_epbase_getaddr (&item->epbase),
              grid_epbase_getaddr (&bitem->epbase), GRID_SOCKADDR_MAX) == 0) {

            /*  Check whether the two sockets are compatible. */
            if (!grid_epbase_ispeer (&item->epbase, bitem->protocol))
                break;

            /*  Call back to cinproc to create actual connection. */
            fn (item, bitem);

            break;
        }
    }

    grid_mutex_unlock (&self.sync);
}

void grid_ins_disconnect (struct grid_ins_item *item)
{
    grid_mutex_lock (&self.sync);
    grid_list_erase (&self.connected, &item->item);
    grid_mutex_unlock (&self.sync);
}

void grid_ins_unbind (struct grid_ins_item *item)
{
    grid_mutex_lock (&self.sync);
    grid_list_erase (&self.bound, &item->item);
    grid_mutex_unlock (&self.sync);
}


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

#include "bus.h"
#include "xbus.h"

#include "../../grid.h"
#include "../../bus.h"

#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/err.h"
#include "../../utils/list.h"

struct grid_bus {
    struct grid_xbus xbus;
};

/*  Private functions. */
static void grid_bus_init (struct grid_bus *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint);
static void grid_bus_term (struct grid_bus *self);

/*  Implementation of grid_sockbase's virtual functions. */
static void grid_bus_destroy (struct grid_sockbase *self);
static int grid_bus_send (struct grid_sockbase *self, struct grid_msg *msg);
static int grid_bus_recv (struct grid_sockbase *self, struct grid_msg *msg);
static const struct grid_sockbase_vfptr grid_bus_sockbase_vfptr = {
    NULL,
    grid_bus_destroy,
    grid_xbus_add,
    grid_xbus_rm,
    grid_xbus_in,
    grid_xbus_out,
    grid_xbus_events,
    grid_bus_send,
    grid_bus_recv,
    grid_xbus_setopt,
    grid_xbus_getopt
};

static void grid_bus_init (struct grid_bus *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint)
{
    grid_xbus_init (&self->xbus, vfptr, hint);
}

static void grid_bus_term (struct grid_bus *self)
{
    grid_xbus_term (&self->xbus);
}

static void grid_bus_destroy (struct grid_sockbase *self)
{
    struct grid_bus *bus;

    bus = grid_cont (self, struct grid_bus, xbus.sockbase);

    grid_bus_term (bus);
    grid_free (bus);
}

static int grid_bus_send (struct grid_sockbase *self, struct grid_msg *msg)
{
    int rc;
    struct grid_bus *bus;

    bus = grid_cont (self, struct grid_bus, xbus.sockbase);

    /*  Check for malformed messages. */
    if (grid_chunkref_size (&msg->sphdr))
        return -EINVAL;

    /*  Send the message. */
    rc = grid_xbus_send (&bus->xbus.sockbase, msg);
    errnum_assert (rc == 0, -rc);

    return 0;
}

static int grid_bus_recv (struct grid_sockbase *self, struct grid_msg *msg)
{
    int rc;
    struct grid_bus *bus;

    bus = grid_cont (self, struct grid_bus, xbus.sockbase);

    /*  Get next message. */
    rc = grid_xbus_recv (&bus->xbus.sockbase, msg);
    if (grid_slow (rc == -EAGAIN))
        return -EAGAIN;
    errnum_assert (rc == 0, -rc);
    grid_assert (grid_chunkref_size (&msg->sphdr) == sizeof (uint64_t));

    /*  Discard the header. */
    grid_chunkref_term (&msg->sphdr);
    grid_chunkref_init (&msg->sphdr, 0);
    
    return 0;
}

static int grid_bus_create (void *hint, struct grid_sockbase **sockbase)
{
    struct grid_bus *self;

    self = grid_alloc (sizeof (struct grid_bus), "socket (bus)");
    alloc_assert (self);
    grid_bus_init (self, &grid_bus_sockbase_vfptr, hint);
    *sockbase = &self->xbus.sockbase;

    return 0;
}

static struct grid_socktype grid_bus_socktype_struct = {
    AF_SP,
    GRID_BUS,
    0,
    grid_bus_create,
    grid_xbus_ispeer,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_socktype *grid_bus_socktype = &grid_bus_socktype_struct;


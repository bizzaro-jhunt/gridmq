/*
    Copyright (c) 2012-2013 Martin Sustrik  All rights reserved.
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

#include "tcp.h"
#include "btcp.h"
#include "ctcp.h"

#include "../../tcp.h"

#include "../utils/port.h"
#include "../utils/iface.h"

#include "../../utils/err.h"
#include "../../utils/alloc.h"
#include "../../utils/fast.h"
#include "../../utils/list.h"
#include "../../utils/cont.h"

#include <string.h>

#if defined GRID_HAVE_WINDOWS
#include "../../utils/win.h"
#else
#include <unistd.h>
#endif

/*  TCP-specific socket options. */

struct grid_tcp_optset {
    struct grid_optset base;
    int nodelay;
};

static void grid_tcp_optset_destroy (struct grid_optset *self);
static int grid_tcp_optset_setopt (struct grid_optset *self, int option,
    const void *optval, size_t optvallen);
static int grid_tcp_optset_getopt (struct grid_optset *self, int option,
    void *optval, size_t *optvallen);
static const struct grid_optset_vfptr grid_tcp_optset_vfptr = {
    grid_tcp_optset_destroy,
    grid_tcp_optset_setopt,
    grid_tcp_optset_getopt
};

/*  grid_transport interface. */
static int grid_tcp_bind (void *hint, struct grid_epbase **epbase);
static int grid_tcp_connect (void *hint, struct grid_epbase **epbase);
static struct grid_optset *grid_tcp_optset (void);

static struct grid_transport grid_tcp_vfptr = {
    "tcp",
    GRID_TCP,
    NULL,
    NULL,
    grid_tcp_bind,
    grid_tcp_connect,
    grid_tcp_optset,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_transport *grid_tcp = &grid_tcp_vfptr;

static int grid_tcp_bind (void *hint, struct grid_epbase **epbase)
{
    return grid_btcp_create (hint, epbase);
}

static int grid_tcp_connect (void *hint, struct grid_epbase **epbase)
{
    return grid_ctcp_create (hint, epbase);
}

static struct grid_optset *grid_tcp_optset ()
{
    struct grid_tcp_optset *optset;

    optset = grid_alloc (sizeof (struct grid_tcp_optset), "optset (tcp)");
    alloc_assert (optset);
    optset->base.vfptr = &grid_tcp_optset_vfptr;

    /*  Default values for TCP socket options. */
    optset->nodelay = 0;

    return &optset->base;   
}

static void grid_tcp_optset_destroy (struct grid_optset *self)
{
    struct grid_tcp_optset *optset;

    optset = grid_cont (self, struct grid_tcp_optset, base);
    grid_free (optset);
}

static int grid_tcp_optset_setopt (struct grid_optset *self, int option,
    const void *optval, size_t optvallen)
{
    struct grid_tcp_optset *optset;
    int val;

    optset = grid_cont (self, struct grid_tcp_optset, base);

    /*  At this point we assume that all options are of type int. */
    if (optvallen != sizeof (int))
        return -EINVAL;
    val = *(int*) optval;

    switch (option) {
    case GRID_TCP_NODELAY:
        if (grid_slow (val != 0 && val != 1))
            return -EINVAL;
        optset->nodelay = val;
        return 0;
    default:
        return -ENOPROTOOPT;
    }
}

static int grid_tcp_optset_getopt (struct grid_optset *self, int option,
    void *optval, size_t *optvallen)
{
    struct grid_tcp_optset *optset;
    int intval;

    optset = grid_cont (self, struct grid_tcp_optset, base);

    switch (option) {
    case GRID_TCP_NODELAY:
        intval = optset->nodelay;
        break;
    default:
        return -ENOPROTOOPT;
    }
    memcpy (optval, &intval,
        *optvallen < sizeof (int) ? *optvallen : sizeof (int));
    *optvallen = sizeof (int);
    return 0;
}


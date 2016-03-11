/*
    Copyright (c) 2012-2013 250bpm s.r.o.  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.
    Copyright (c) 2014 Wirebird Labs LLC.  All rights reserved.
    Copyright 2015 Garrett D'Amore <garrett@damore.org>

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

#include "ws.h"
#include "bws.h"
#include "cws.h"
#include "sws.h"

#include "../../ws.h"

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

/*  WebSocket-specific socket options. */
struct grid_ws_optset {
    struct grid_optset base;
    int msg_type;
};

static void grid_ws_optset_destroy (struct grid_optset *self);
static int grid_ws_optset_setopt (struct grid_optset *self, int option,
    const void *optval, size_t optvallen);
static int grid_ws_optset_getopt (struct grid_optset *self, int option,
    void *optval, size_t *optvallen);
static const struct grid_optset_vfptr grid_ws_optset_vfptr = {
    grid_ws_optset_destroy,
    grid_ws_optset_setopt,
    grid_ws_optset_getopt
};

/*  grid_transport interface. */
static int grid_ws_bind (void *hint, struct grid_epbase **epbase);
static int grid_ws_connect (void *hint, struct grid_epbase **epbase);
static struct grid_optset *grid_ws_optset (void);

static struct grid_transport grid_ws_vfptr = {
    "ws",
    GRID_WS,
    NULL,
    NULL,
    grid_ws_bind,
    grid_ws_connect,
    grid_ws_optset,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_transport *grid_ws = &grid_ws_vfptr;

static int grid_ws_bind (void *hint, struct grid_epbase **epbase)
{
    return grid_bws_create (hint, epbase);
}

static int grid_ws_connect (void *hint, struct grid_epbase **epbase)
{
    return grid_cws_create (hint, epbase); 
}

static struct grid_optset *grid_ws_optset ()
{
    struct grid_ws_optset *optset;

    optset = grid_alloc (sizeof (struct grid_ws_optset), "optset (ws)");
    alloc_assert (optset);
    optset->base.vfptr = &grid_ws_optset_vfptr;

    /*  Default values for WebSocket options. */
    optset->msg_type = GRID_WS_MSG_TYPE_BINARY;

    return &optset->base;   
}

static void grid_ws_optset_destroy (struct grid_optset *self)
{
    struct grid_ws_optset *optset;

    optset = grid_cont (self, struct grid_ws_optset, base);
    grid_free (optset);
}

static int grid_ws_optset_setopt (struct grid_optset *self, int option,
    const void *optval, size_t optvallen)
{
    struct grid_ws_optset *optset;
    int val;

    optset = grid_cont (self, struct grid_ws_optset, base);
    if (optvallen != sizeof (int)) {
        return -EINVAL;
    }
    val = *(int *)optval;

    switch (option) {
    case GRID_WS_MSG_TYPE:
        switch (val) {
        case GRID_WS_MSG_TYPE_TEXT:
        case GRID_WS_MSG_TYPE_BINARY:
	    optset->msg_type = val;
            return 0;
        default:
            return -EINVAL;
        }
    default:
        return -ENOPROTOOPT;
    }
}

static int grid_ws_optset_getopt (struct grid_optset *self, int option,
    void *optval, size_t *optvallen)
{
    struct grid_ws_optset *optset;

    optset = grid_cont (self, struct grid_ws_optset, base);

    switch (option) {
    case GRID_WS_MSG_TYPE:
        memcpy (optval, &optset->msg_type,
            *optvallen < sizeof (int) ? *optvallen : sizeof (int));
        *optvallen = sizeof (int);
        return 0;
    default:
        return -ENOPROTOOPT;
    }
}

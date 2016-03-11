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

#include "inproc.h"
#include "ins.h"
#include "binproc.h"
#include "cinproc.h"

#include "../../inproc.h"

#include <string.h>

/*  grid_transport interface. */
static void grid_inproc_init (void);
static void grid_inproc_term (void);
static int grid_inproc_bind (void *hint, struct grid_epbase **epbase);
static int grid_inproc_connect (void *hint, struct grid_epbase **epbase);

static struct grid_transport grid_inproc_vfptr = {
    "inproc",
    GRID_INPROC,
    grid_inproc_init,
    grid_inproc_term,
    grid_inproc_bind,
    grid_inproc_connect,
    NULL,
    GRID_LIST_ITEM_INITIALIZER
};

struct grid_transport *grid_inproc = &grid_inproc_vfptr;

static void grid_inproc_init (void)
{
    grid_ins_init ();
}

static void grid_inproc_term (void)
{
    grid_ins_term ();
}

static int grid_inproc_bind (void *hint, struct grid_epbase **epbase)
{
    return grid_binproc_create (hint, epbase);
}

static int grid_inproc_connect (void *hint, struct grid_epbase **epbase)
{
    return grid_cinproc_create (hint, epbase);
}


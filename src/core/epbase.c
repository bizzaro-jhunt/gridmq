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

#include "../transport.h"

#include "ep.h"
#include "sock.h"
#include "../utils/attr.h"

void grid_epbase_init (struct grid_epbase *self,
    const struct grid_epbase_vfptr *vfptr, void *hint)
{
    self->vfptr = vfptr;
    self->ep = (struct grid_ep*) hint;
}

void grid_epbase_term (GRID_UNUSED struct grid_epbase *self)
{
}

void grid_epbase_stopped (struct grid_epbase *self)
{
    grid_ep_stopped (self->ep);
}

struct grid_ctx *grid_epbase_getctx (struct grid_epbase *self)
{
    return grid_ep_getctx (self->ep);
}

const char *grid_epbase_getaddr (struct grid_epbase *self)
{
    return grid_ep_getaddr (self->ep);
}

void grid_epbase_getopt (struct grid_epbase *self, int level, int option,
    void *optval, size_t *optvallen)
{
    grid_ep_getopt (self->ep, level, option, optval, optvallen);
}

int grid_epbase_ispeer (struct grid_epbase *self, int socktype)
{
    return grid_ep_ispeer (self->ep, socktype);
}

void grid_epbase_set_error (struct grid_epbase *self, int errnum)
{
    grid_ep_set_error (self->ep, errnum);
}

void grid_epbase_clear_error (struct grid_epbase *self)
{
    grid_ep_clear_error (self->ep);
}

void grid_epbase_stat_increment(struct grid_epbase *self, int name, int increment) {
    grid_ep_stat_increment(self->ep, name, increment);
}

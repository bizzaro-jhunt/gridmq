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

#include "excl.h"

#include "../../utils/fast.h"
#include "../../utils/err.h"
#include "../../utils/attr.h"

void grid_excl_init (struct grid_excl *self)
{
    self->pipe = NULL;
    self->inpipe = NULL;
    self->outpipe = NULL;
}

void grid_excl_term (struct grid_excl *self)
{
    grid_assert (!self->pipe);
    grid_assert (!self->inpipe);
    grid_assert (!self->outpipe);
}

int grid_excl_add (struct grid_excl *self, struct grid_pipe *pipe)
{
    /*  If there's a connection being used, reject any new connection. */
    if (self->pipe)
        return -EISCONN;

    /*  Remember that this pipe is the active one. */
    self->pipe = pipe;

    return 0;
}

void grid_excl_rm (struct grid_excl *self, GRID_UNUSED struct grid_pipe *pipe)
{
   grid_assert (self->pipe);
   self->pipe = NULL;
   self->inpipe = NULL;
   self->outpipe = NULL;
}

void grid_excl_in (struct grid_excl *self, struct grid_pipe *pipe)
{
    grid_assert (!self->inpipe);
    grid_assert (pipe == self->pipe);
    self->inpipe = pipe;
}

void grid_excl_out (struct grid_excl *self, struct grid_pipe *pipe)
{
    grid_assert (!self->outpipe);
    grid_assert (pipe == self->pipe);
    self->outpipe = pipe;
}

int grid_excl_send (struct grid_excl *self, struct grid_msg *msg)
{
    int rc;

    if (grid_slow (!self->outpipe))
        return -EAGAIN;

    rc = grid_pipe_send (self->outpipe, msg);
    errnum_assert (rc >= 0, -rc);

    if (rc & GRID_PIPE_RELEASE)
        self->outpipe = NULL;

    return rc & ~GRID_PIPE_RELEASE;
}

int grid_excl_recv (struct grid_excl *self, struct grid_msg *msg)
{
    int rc;

    if (grid_slow (!self->inpipe))
        return -EAGAIN;

    rc = grid_pipe_recv (self->inpipe, msg);
    errnum_assert (rc >= 0, -rc);

    if (rc & GRID_PIPE_RELEASE)
        self->inpipe = NULL;

    return rc & ~GRID_PIPE_RELEASE;
}

int grid_excl_can_send (struct grid_excl *self)
{
    return self->outpipe ? 1 : 0;
}

int grid_excl_can_recv (struct grid_excl *self)
{
    return self->inpipe ? 1 : 0;
}


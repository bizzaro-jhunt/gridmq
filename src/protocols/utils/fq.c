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

#include "fq.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"

#include <stddef.h>

void grid_fq_init (struct grid_fq *self)
{
    grid_priolist_init (&self->priolist);
}

void grid_fq_term (struct grid_fq *self)
{
    grid_priolist_term (&self->priolist);
}

void grid_fq_add (struct grid_fq *self, struct grid_fq_data *data,
    struct grid_pipe *pipe, int priority)
{
    grid_priolist_add (&self->priolist, &data->priodata, pipe, priority);
}

void grid_fq_rm (struct grid_fq *self, struct grid_fq_data *data)
{
    grid_priolist_rm (&self->priolist, &data->priodata);
}

void grid_fq_in (struct grid_fq *self, struct grid_fq_data *data)
{
    grid_priolist_activate (&self->priolist, &data->priodata);
}

int grid_fq_can_recv (struct grid_fq *self)
{
    return grid_priolist_is_active (&self->priolist);
}

int grid_fq_recv (struct grid_fq *self, struct grid_msg *msg, struct grid_pipe **pipe)
{
    int rc;
    struct grid_pipe *p;

    /*  Pipe is NULL only when there are no avialable pipes. */
    p = grid_priolist_getpipe (&self->priolist);
    if (grid_slow (!p))
        return -EAGAIN;

    /*  Receive the messsage. */
    rc = grid_pipe_recv (p, msg);
    errnum_assert (rc >= 0, -rc);

    /*  Return the pipe data to the user, if required. */
    if (pipe)
        *pipe = p;

    /*  Move to the next pipe. */
    grid_priolist_advance (&self->priolist, rc & GRID_PIPE_RELEASE);

    return rc & ~GRID_PIPE_RELEASE;
}


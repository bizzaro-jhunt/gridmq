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

#include "lb.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"

#include <stddef.h>

void grid_lb_init (struct grid_lb *self)
{
    grid_priolist_init (&self->priolist);
}

void grid_lb_term (struct grid_lb *self)
{
    grid_priolist_term (&self->priolist);
}

void grid_lb_add (struct grid_lb *self, struct grid_lb_data *data,
    struct grid_pipe *pipe, int priority)
{
    grid_priolist_add (&self->priolist, &data->priodata, pipe, priority);
}

void grid_lb_rm (struct grid_lb *self, struct grid_lb_data *data)
{
    grid_priolist_rm (&self->priolist, &data->priodata);
}

void grid_lb_out (struct grid_lb *self, struct grid_lb_data *data)
{
    grid_priolist_activate (&self->priolist, &data->priodata);
}

int grid_lb_can_send (struct grid_lb *self)
{
    return grid_priolist_is_active (&self->priolist);
}

int grid_lb_get_priority (struct grid_lb *self)
{
    return grid_priolist_get_priority (&self->priolist);
}

int grid_lb_send (struct grid_lb *self, struct grid_msg *msg, struct grid_pipe **to)
{
    int rc;
    struct grid_pipe *pipe;

    /*  Pipe is NULL only when there are no avialable pipes. */
    pipe = grid_priolist_getpipe (&self->priolist);
    if (grid_slow (!pipe))
        return -EAGAIN;

    /*  Send the messsage. */
    rc = grid_pipe_send (pipe, msg);
    errnum_assert (rc >= 0, -rc);

    /*  Move to the next pipe. */
    grid_priolist_advance (&self->priolist, rc & GRID_PIPE_RELEASE);

    if (to != NULL)
        *to = pipe;

    return rc & ~GRID_PIPE_RELEASE;
}


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

#ifndef GRID_XREQ_INCLUDED
#define GRID_XREQ_INCLUDED

#include "../../protocol.h"

#include "../utils/lb.h"
#include "../utils/fq.h"

struct grid_xreq {
    struct grid_sockbase sockbase;
    struct grid_lb lb;
    struct grid_fq fq;
};

void grid_xreq_init (struct grid_xreq *self, const struct grid_sockbase_vfptr *vfptr,
    void *hint);
void grid_xreq_term (struct grid_xreq *self);

int grid_xreq_add (struct grid_sockbase *self, struct grid_pipe *pipe);
void grid_xreq_rm (struct grid_sockbase *self, struct grid_pipe *pipe);
void grid_xreq_in (struct grid_sockbase *self, struct grid_pipe *pipe);
void grid_xreq_out (struct grid_sockbase *self, struct grid_pipe *pipe);
int grid_xreq_events (struct grid_sockbase *self);
int grid_xreq_send (struct grid_sockbase *self, struct grid_msg *msg);
int grid_xreq_send_to (struct grid_sockbase *self, struct grid_msg *msg,
    struct grid_pipe **to);
int grid_xreq_recv (struct grid_sockbase *self, struct grid_msg *msg);
int grid_xreq_setopt (struct grid_sockbase *self, int level, int option,
    const void *optval, size_t optvallen);
int grid_xreq_getopt (struct grid_sockbase *self, int level, int option,
    void *optval, size_t *optvallen);

int grid_xreq_ispeer (int socktype);

extern struct grid_socktype *grid_xreq_socktype;

#endif

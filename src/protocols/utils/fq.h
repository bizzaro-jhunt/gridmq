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

#ifndef GRID_FQ_INCLUDED
#define GRID_FQ_INCLUDED

#include "../../protocol.h"

#include "priolist.h"

/*  Fair-queuer. Retrieves messages from a set of pipes in round-robin
    manner. */

struct grid_fq_data {
    struct grid_priolist_data priodata;
};

struct grid_fq {
    struct grid_priolist priolist;
};

void grid_fq_init (struct grid_fq *self);
void grid_fq_term (struct grid_fq *self);
void grid_fq_add (struct grid_fq *self, struct grid_fq_data *data,
    struct grid_pipe *pipe, int priority);
void grid_fq_rm (struct grid_fq *self, struct grid_fq_data *data);
void grid_fq_in (struct grid_fq *self, struct grid_fq_data *data);
int grid_fq_can_recv (struct grid_fq *self);
int grid_fq_recv (struct grid_fq *self, struct grid_msg *msg, struct grid_pipe **pipe);

#endif

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

#ifndef GRID_LB_INCLUDED
#define GRID_LB_INCLUDED

#include "../../protocol.h"

#include "priolist.h"

/*  A load balancer. Round-robins messages to a set of pipes. */

struct grid_lb_data {
    struct grid_priolist_data priodata;
};

struct grid_lb {
    struct grid_priolist priolist;
};

void grid_lb_init (struct grid_lb *self);
void grid_lb_term (struct grid_lb *self);
void grid_lb_add (struct grid_lb *self, struct grid_lb_data *data,
    struct grid_pipe *pipe, int priority);
void grid_lb_rm (struct grid_lb *self, struct grid_lb_data *data);
void grid_lb_out (struct grid_lb *self, struct grid_lb_data *data);
int grid_lb_can_send (struct grid_lb *self);
int grid_lb_get_priority (struct grid_lb *self);
int grid_lb_send (struct grid_lb *self, struct grid_msg *msg, struct grid_pipe **to);

#endif

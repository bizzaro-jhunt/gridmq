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

#ifndef GRID_DIST_INCLUDED
#define GRID_DIST_INCLUDED

#include "../../protocol.h"

#include "../../utils/list.h"

/*  Distributor. Sends messages to all the pipes. */

struct grid_dist_data {
    struct grid_list_item item;
    struct grid_pipe *pipe;
};

struct grid_dist {
    uint32_t count;
    struct grid_list pipes;
};

void grid_dist_init (struct grid_dist *self);
void grid_dist_term (struct grid_dist *self);
void grid_dist_add (struct grid_dist *self, 
    struct grid_dist_data *data, struct grid_pipe *pipe);
void grid_dist_rm (struct grid_dist *self, struct grid_dist_data *data);
void grid_dist_out (struct grid_dist *self, struct grid_dist_data *data);

/*  Sends the message to all the attached pipes except the one specified
    by 'exclude' parameter. If 'exclude' is NULL, message is sent to all
    attached pipes. */
int grid_dist_send (struct grid_dist *self, struct grid_msg *msg,
    struct grid_pipe *exclude);

#endif

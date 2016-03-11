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

#ifndef GRID_EXCL_INCLUDED
#define GRID_EXCL_INCLUDED

#include "../../protocol.h"

#include <stddef.h>

/*  This is an object to handle a single pipe. To be used by socket types that
    can work with precisely one connection, e.g. PAIR. */

struct grid_excl {

    /*  The pipe being used at the moment. All other pipes will be rejected
        until this one terminates. NULL if there is no connected pipe. */
    struct grid_pipe *pipe;

    /*  Pipe ready for receiving. It's either equal to 'pipe' or NULL. */
    struct grid_pipe *inpipe;

    /*  Pipe ready for sending. It's either equal to 'pipe' or NULL. */
    struct grid_pipe *outpipe;

};

void grid_excl_init (struct grid_excl *self);
void grid_excl_term (struct grid_excl *self);
int grid_excl_add (struct grid_excl *self, struct grid_pipe *pipe);
void grid_excl_rm (struct grid_excl *self, struct grid_pipe *pipe);
void grid_excl_in (struct grid_excl *self, struct grid_pipe *pipe);
void grid_excl_out (struct grid_excl *self, struct grid_pipe *pipe);
int grid_excl_send (struct grid_excl *self, struct grid_msg *msg);
int grid_excl_recv (struct grid_excl *self, struct grid_msg *msg);
int grid_excl_can_send (struct grid_excl *self);
int grid_excl_can_recv (struct grid_excl *self);

#endif

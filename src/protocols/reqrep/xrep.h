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

#ifndef GRID_XREP_INCLUDED
#define GRID_XREP_INCLUDED

#include "../../protocol.h"

#include "../../utils/hash.h"
#include "../../utils/int.h"

#include "../utils/fq.h"

#include <stddef.h>

#define GRID_XREP_OUT 1

struct grid_xrep_data {
    struct grid_pipe *pipe;
    struct grid_hash_item outitem;
    struct grid_fq_data initem;
    uint32_t flags;
};

struct grid_xrep {

    struct grid_sockbase sockbase;

    /*  Key to be assigned to the next added pipe. */
    uint32_t next_key;

    /*  Map of all registered pipes indexed by the peer ID. */
    struct grid_hash outpipes;

    /*  Fair-queuer to get messages from. */
    struct grid_fq inpipes;
};

void grid_xrep_init (struct grid_xrep *self, const struct grid_sockbase_vfptr *vfptr,
    void *hint);
void grid_xrep_term (struct grid_xrep *self);

int grid_xrep_add (struct grid_sockbase *self, struct grid_pipe *pipe);
void grid_xrep_rm (struct grid_sockbase *self, struct grid_pipe *pipe);
void grid_xrep_in (struct grid_sockbase *self, struct grid_pipe *pipe);
void grid_xrep_out (struct grid_sockbase *self, struct grid_pipe *pipe);
int grid_xrep_events (struct grid_sockbase *self);
int grid_xrep_send (struct grid_sockbase *self, struct grid_msg *msg);
int grid_xrep_recv (struct grid_sockbase *self, struct grid_msg *msg);
int grid_xrep_setopt (struct grid_sockbase *self, int level, int option,
    const void *optval, size_t optvallen);
int grid_xrep_getopt (struct grid_sockbase *self, int level, int option,
    void *optval, size_t *optvallen);

int grid_xrep_ispeer (int socktype);

extern struct grid_socktype *grid_xrep_socktype;

#endif

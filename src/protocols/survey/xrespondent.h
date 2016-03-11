/*
    Copyright (c) 201-2013 Martin Sustrik  All rights reserved.
    Copyright 2015 Garrett D'Amore <garrett@damore.org>

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

#ifndef GRID_XRESPONDENT_INCLUDED
#define GRID_XRESPONDENT_INCLUDED

#include "../../protocol.h"

#include "../../utils/hash.h"
#include "../../utils/int.h"
#include "../utils/fq.h"

extern struct grid_socktype *grid_xrespondent_socktype;

#define GRID_XRESPONDENT_OUT 1

struct grid_xrespondent_data {
    struct grid_pipe *pipe;
    struct grid_hash_item outitem;
    struct grid_fq_data initem;
    uint32_t flags;
};

struct grid_xrespondent {
    struct grid_sockbase sockbase;

    /*  Key to be assigned to the next added pipe. */
    uint32_t next_key;

    /*  Map of all registered pipes indexed by the peer ID. */
    struct grid_hash outpipes;

    /*  Fair-queuer to get surveys from. */
    struct grid_fq inpipes;
};

void grid_xrespondent_init (struct grid_xrespondent *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint);
void grid_xrespondent_term (struct grid_xrespondent *self);

int grid_xrespondent_add (struct grid_sockbase *self, struct grid_pipe *pipe);
void grid_xrespondent_rm (struct grid_sockbase *self, struct grid_pipe *pipe);
void grid_xrespondent_in (struct grid_sockbase *self, struct grid_pipe *pipe);
void grid_xrespondent_out (struct grid_sockbase *self, struct grid_pipe *pipe);
int grid_xrespondent_events (struct grid_sockbase *self);
int grid_xrespondent_send (struct grid_sockbase *self, struct grid_msg *msg);
int grid_xrespondent_recv (struct grid_sockbase *self, struct grid_msg *msg);
int grid_xrespondent_setopt (struct grid_sockbase *self, int level, int option,
    const void *optval, size_t optvallen);
int grid_xrespondent_getopt (struct grid_sockbase *self, int level, int option,
    void *optval, size_t *optvallen);

int grid_xrespondent_ispeer (int socktype);

#endif

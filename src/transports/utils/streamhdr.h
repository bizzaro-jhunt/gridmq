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

#ifndef GRID_STREAMHDR_INCLUDED
#define GRID_STREAMHDR_INCLUDED

#include "../../transport.h"

#include "../../aio/fsm.h"
#include "../../aio/usock.h"
#include "../../aio/timer.h"

#include "../../utils/int.h"

/*  This state machine exchanges protocol headers on top of
    a stream-based bi-directional connection. */

#define GRID_STREAMHDR_OK 1
#define GRID_STREAMHDR_ERROR 2
#define GRID_STREAMHDR_STOPPED 3

struct grid_streamhdr {

    /*  The state machine. */
    struct grid_fsm fsm;
    int state;

    /*  Used to timeout the protocol header exchange. */
    struct grid_timer timer;

    /*  The underlying socket. */
    struct grid_usock *usock;

    /*  The original owner of the underlying socket. */
    struct grid_fsm_owner usock_owner;

    /*  Handle to the pipe. */
    struct grid_pipebase *pipebase;

    /*  Protocol header. */
    uint8_t protohdr [8];

    /*  Event fired when the state machine ends. */
    struct grid_fsm_event done;
};

void grid_streamhdr_init (struct grid_streamhdr *self, int src,
    struct grid_fsm *owner);
void grid_streamhdr_term (struct grid_streamhdr *self);

int grid_streamhdr_isidle (struct grid_streamhdr *self);
void grid_streamhdr_start (struct grid_streamhdr *self, struct grid_usock *usock,
    struct grid_pipebase *pipebase);
void grid_streamhdr_stop (struct grid_streamhdr *self);

#endif

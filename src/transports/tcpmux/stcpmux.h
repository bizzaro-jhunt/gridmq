/*
    Copyright (c) 2013-2014 Martin Sustrik  All rights reserved.

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

#ifndef GRID_STCPMUX_INCLUDED
#define GRID_STCPMUX_INCLUDED

#include "../../transport.h"

#include "../../aio/fsm.h"
#include "../../aio/usock.h"

#include "../utils/streamhdr.h"

#include "../../utils/msg.h"

/*  This state machine handles TCPMUX connection from the point where it is
    established to the point when it is broken. */

#define GRID_STCPMUX_ERROR 1
#define GRID_STCPMUX_STOPPED 2

struct grid_stcpmux {

    /*  The state machine. */
    struct grid_fsm fsm;
    int state;

    /*  The underlying socket. */
    struct grid_usock *usock;

    /*  Child state machine to do protocol header exchange. */
    struct grid_streamhdr streamhdr;

    /*  The original owner of the underlying socket. */
    struct grid_fsm_owner usock_owner;

    /*  Pipe connecting this TCPMUX connection to the gridmq core. */
    struct grid_pipebase pipebase;

    /*  State of inbound state machine. */
    int instate;

    /*  Buffer used to store the header of incoming message. */
    uint8_t inhdr [8];

    /*  Message being received at the moment. */
    struct grid_msg inmsg;

    /*  State of the outbound state machine. */
    int outstate;

    /*  Buffer used to store the header of outgoing message. */
    uint8_t outhdr [8];

    /*  Message being sent at the moment. */
    struct grid_msg outmsg;

    /*  Event raised when the state machine ends. */
    struct grid_fsm_event done;
};

void grid_stcpmux_init (struct grid_stcpmux *self, int src,
    struct grid_epbase *epbase, struct grid_fsm *owner);
void grid_stcpmux_term (struct grid_stcpmux *self);

int grid_stcpmux_isidle (struct grid_stcpmux *self);
void grid_stcpmux_start (struct grid_stcpmux *self, struct grid_usock *usock);
void grid_stcpmux_stop (struct grid_stcpmux *self);

#endif


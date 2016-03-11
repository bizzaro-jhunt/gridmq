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

#ifndef GRID_SINPROC_INCLUDED
#define GRID_SINPROC_INCLUDED

#include "msgqueue.h"

#include "../../transport.h"

#include "../../aio/fsm.h"

#include "../../utils/msg.h"
#include "../../utils/list.h"

#define GRID_SINPROC_CONNECT 1
#define GRID_SINPROC_READY 2
#define GRID_SINPROC_ACCEPTED 3
#define GRID_SINPROC_SENT 4
#define GRID_SINPROC_RECEIVED 5
#define GRID_SINPROC_DISCONNECT 6
#define GRID_SINPROC_STOPPED 7

/*  We use a random value here to prevent accidental clashes with the peer's
    internal source IDs. */
#define GRID_SINPROC_SRC_PEER 27713

struct grid_sinproc {

    /*  The state machine. */
    struct grid_fsm fsm;
    int state;

    /*  Any combination of the flags defined in the .c file. */
    int flags;

    /*  Pointer to the peer inproc session, if connected. NULL otherwise. */
    struct grid_sinproc *peer;

    /*  Pipe connecting this inproc connection to the gridmq core. */
    struct grid_pipebase pipebase;

    /*  Inbound message queue. The messages contained are meant to be received
        by the user later on. */
    struct grid_msgqueue msgqueue;

    /*  This message is the one being sent from this session to the peer
        session. It holds the data only temporarily, until the peer moves
        it to its msgqueue. */
    struct grid_msg msg;

    /*  Outbound events. I.e. event sent by this sinproc to the peer sinproc. */
    struct grid_fsm_event event_connect;

    /*  Inbound events. I.e. events sent by the peer sinproc to this inproc. */
    struct grid_fsm_event event_sent;
    struct grid_fsm_event event_received;
    struct grid_fsm_event event_disconnect;

    /*  This member is used only if we are on the bound side. binproc object
        has a list of sinprocs it handles. */
    struct grid_list_item item;
};

void grid_sinproc_init (struct grid_sinproc *self, int src,
    struct grid_epbase *epbase, struct grid_fsm *owner);
void grid_sinproc_term (struct grid_sinproc *self);
int grid_sinproc_isidle (struct grid_sinproc *self);

/*  Connect and accept are two different ways to start the state machine. */
void grid_sinproc_connect (struct grid_sinproc *self, struct grid_fsm *peer);
void grid_sinproc_accept (struct grid_sinproc *self, struct grid_sinproc *peer);
void grid_sinproc_stop (struct grid_sinproc *self);

#endif

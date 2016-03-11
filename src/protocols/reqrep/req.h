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

#ifndef GRID_REQ_INCLUDED
#define GRID_REQ_INCLUDED

#include "xreq.h"
#include "task.h"

#include "../../protocol.h"
#include "../../aio/fsm.h"

struct grid_req {

    /*  The base class. Raw REQ socket. */
    struct grid_xreq xreq;

    /*  The state machine. */
    struct grid_fsm fsm;
    int state;

    /*  Last request ID assigned. */
    uint32_t lastid;

    /*  Protocol-specific socket options. */
    int resend_ivl;

    /*  The request being processed. */
    struct grid_task task;
};

extern struct grid_socktype *grid_req_socktype;

/*  Some users may want to extend the REQ protocol similar to how REQ extends XREQ.
    Expose these methods to improve extensibility. */
void grid_req_init (struct grid_req *self,
    const struct grid_sockbase_vfptr *vfptr, void *hint);
void grid_req_term (struct grid_req *self);
int grid_req_inprogress (struct grid_req *self);
void grid_req_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
void grid_req_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);
void grid_req_action_send (struct grid_req *self, int allow_delay);

/*  Implementation of grid_sockbase's virtual functions. */
void grid_req_stop (struct grid_sockbase *self);
void grid_req_destroy (struct grid_sockbase *self);
void grid_req_in (struct grid_sockbase *self, struct grid_pipe *pipe);
void grid_req_out (struct grid_sockbase *self, struct grid_pipe *pipe);
int grid_req_events (struct grid_sockbase *self);
int grid_req_csend (struct grid_sockbase *self, struct grid_msg *msg);
void grid_req_rm (struct grid_sockbase *self, struct grid_pipe *pipe);
int grid_req_crecv (struct grid_sockbase *self, struct grid_msg *msg);
int grid_req_setopt (struct grid_sockbase *self, int level, int option,
    const void *optval, size_t optvallen);
int grid_req_getopt (struct grid_sockbase *self, int level, int option,
    void *optval, size_t *optvallen);
int grid_req_csend (struct grid_sockbase *self, struct grid_msg *msg);
int grid_req_crecv (struct grid_sockbase *self, struct grid_msg *msg);

#endif

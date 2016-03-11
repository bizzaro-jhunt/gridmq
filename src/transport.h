/*
    Copyright (c) 2012-2014 Martin Sustrik  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.

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

#ifndef GRID_TRANSPORT_INCLUDED
#define GRID_TRANSPORT_INCLUDED

#include "grid.h"

#include "aio/fsm.h"

#include "utils/list.h"
#include "utils/msg.h"
#include "utils/int.h"

#include <stddef.h>

/*  This is the API between the gridmq core and individual transports. */

struct grid_sock;
struct grid_cp;

/******************************************************************************/
/*  Container for transport-specific socket options.                          */
/******************************************************************************/

struct grid_optset;

struct grid_optset_vfptr {
    void (*destroy) (struct grid_optset *self);
    int (*setopt) (struct grid_optset *self, int option, const void *optval,
        size_t optvallen);
    int (*getopt) (struct grid_optset *self, int option, void *optval,
        size_t *optvallen);
};

struct grid_optset {
    const struct grid_optset_vfptr *vfptr;
};

/******************************************************************************/
/*  The base class for endpoints.                                             */
/******************************************************************************/

/*  The best way to think about endpoints is that endpoint is an object created
    by each grid_bind() or grid_connect() call. Each endpoint is associated with
    exactly one address string (e.g. "tcp://127.0.0.1:5555"). */

struct grid_epbase;

struct grid_epbase_vfptr {

    /*  Ask the endpoint to stop itself. The endpoint is allowed to linger
        to send the pending outbound data. When done, it reports the fact by
        invoking grid_epbase_stopped() function. */
    void (*stop) (struct grid_epbase *self);

    /*  Deallocate the endpoint object. */
    void (*destroy) (struct grid_epbase *self);
};

struct grid_epbase {
    const struct grid_epbase_vfptr *vfptr;
    struct grid_ep *ep;
};

/*  Creates a new endpoint. 'hint' parameter is an opaque value that
    was passed to transport's bind or connect function. */
void grid_epbase_init (struct grid_epbase *self,
    const struct grid_epbase_vfptr *vfptr, void *hint);

/*  Notify the user that stopping is done. */
void grid_epbase_stopped (struct grid_epbase *self);

/*  Terminate the epbase object. */
void grid_epbase_term (struct grid_epbase *self);

/*  Returns the AIO context associated with the endpoint. */
struct grid_ctx *grid_epbase_getctx (struct grid_epbase *self);

/*  Returns the address string associated with this endpoint. */
const char *grid_epbase_getaddr (struct grid_epbase *self);

/*  Retrieve value of a socket option. */
void grid_epbase_getopt (struct grid_epbase *self, int level, int option,
    void *optval, size_t *optvallen);

/*  Returns 1 is the specified socket type is a valid peer for this socket,
    or 0 otherwise. */
int grid_epbase_ispeer (struct grid_epbase *self, int socktype);

/*  Notifies a monitoring system the error on this endpoint  */
void grid_epbase_set_error(struct grid_epbase *self, int errnum);

/*  Notifies a monitoring system that error is gone  */
void grid_epbase_clear_error(struct grid_epbase *self);

/*  Increments statistics counters in the socket structure  */
void grid_epbase_stat_increment(struct grid_epbase *self, int name, int increment);


#define GRID_STAT_ESTABLISHED_CONNECTIONS 101
#define GRID_STAT_ACCEPTED_CONNECTIONS    102
#define GRID_STAT_DROPPED_CONNECTIONS     103
#define GRID_STAT_BROKEN_CONNECTIONS      104
#define GRID_STAT_CONNECT_ERRORS          105
#define GRID_STAT_BIND_ERRORS             106
#define GRID_STAT_ACCEPT_ERRORS           107

#define GRID_STAT_CURRENT_CONNECTIONS     201
#define GRID_STAT_INPROGRESS_CONNECTIONS  202
#define GRID_STAT_CURRENT_EP_ERRORS       203


/******************************************************************************/
/*  The base class for pipes.                                                 */
/******************************************************************************/

/*  Pipe represents one "connection", i.e. perfectly ordered uni- or
    bi-directional stream of messages. One endpoint can create multiple pipes
    (for example, bound TCP socket is an endpoint, individual accepted TCP
    connections are represented by pipes. */

struct grid_pipebase;

/*  This value is returned by pipe's send and recv functions to signalise that
    more sends/recvs are not possible at the moment. From that moment on,
    the core will stop invoking the function. To re-establish the message
    flow grid_pipebase_received (respectively grid_pipebase_sent) should
    be called. */
#define GRID_PIPEBASE_RELEASE 1

/*  Specifies that received message is already split into header and body.
    This flag is used only by inproc transport to avoid merging and re-splitting
    the messages passed with a single process. */
#define GRID_PIPEBASE_PARSED 2

struct grid_pipebase_vfptr {

    /*  Send a message to the network. The function can return either error
        (negative number) or any combination of the flags defined above. */
    int (*send) (struct grid_pipebase *self, struct grid_msg *msg);

    /*  Receive a message from the network. The function can return either error
        (negative number) or any combination of the flags defined above. */
    int (*recv) (struct grid_pipebase *self, struct grid_msg *msg);
};

/*  Endpoint specific options. Same restrictions as for grid_pipebase apply  */
struct grid_ep_options
{
    int sndprio;
    int rcvprio;
    int ipv4only;
};

/*  The member of this structure are used internally by the core. Never use
    or modify them directly from the transport. */
struct grid_pipebase {
    struct grid_fsm fsm;
    const struct grid_pipebase_vfptr *vfptr;
    uint8_t state;
    uint8_t instate;
    uint8_t outstate;
    struct grid_sock *sock;
    void *data;
    struct grid_fsm_event in;
    struct grid_fsm_event out;
    struct grid_ep_options options;
};

/*  Initialise the pipe.  */
void grid_pipebase_init (struct grid_pipebase *self,
    const struct grid_pipebase_vfptr *vfptr, struct grid_epbase *epbase);

/*  Terminate the pipe. */
void grid_pipebase_term (struct grid_pipebase *self);

/*  Call this function once the connection is established. */
int grid_pipebase_start (struct grid_pipebase *self);

/*  Call this function once the connection is broken. */
void grid_pipebase_stop (struct grid_pipebase *self);

/*  Call this function when new message was fully received. */
void grid_pipebase_received (struct grid_pipebase *self);

/*  Call this function when current outgoing message was fully sent. */
void grid_pipebase_sent (struct grid_pipebase *self);

/*  Retrieve value of a socket option. */
void grid_pipebase_getopt (struct grid_pipebase *self, int level, int option,
    void *optval, size_t *optvallen);

/*  Returns 1 is the specified socket type is a valid peer for this socket,
    or 0 otherwise. */
int grid_pipebase_ispeer (struct grid_pipebase *self, int socktype);

/******************************************************************************/
/*  The transport class.                                                      */
/******************************************************************************/

struct grid_transport {

    /*  Name of the transport as it appears in the connection strings ("tcp",
        "ipc", "inproc" etc. */
    const char *name;

    /*  ID of the transport. */
    int id;

    /*  Following methods are guarded by a global critical section. Two of these
        function will never be invoked in parallel. The first is called when
        the library is initialised, the second one when it is terminated, i.e.
        when there are no more open sockets. Either of them can be set to NULL
        if no specific initialisation/termination is needed. */
    void (*init) (void);
    void (*term) (void);

    /*  Each of these functions creates an endpoint and returns the newly
        created endpoint in 'epbase' parameter. 'hint' is in opaque pointer
        to be passed to grid_epbase_init(). The epbase object can then be used
        to retrieve the address to bind/connect to. These functions are guarded
        by a socket-wide critical section. Two of these function will never be
        invoked in parallel on the same socket. */
    int (*bind) (void *hint, struct grid_epbase **epbase);
    int (*connect) (void *hint, struct grid_epbase **epbase);

    /*  Create an object to hold transport-specific socket options.
        Set this member to NULL in case there are no transport-specific
        socket options available. */
    struct grid_optset *(*optset) (void);

    /*  This member is used exclusively by the core. Never touch it directly
        from the transport. */
    struct grid_list_item item;
};

#endif

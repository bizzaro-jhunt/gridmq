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

#ifndef GRID_SOCK_INCLUDED
#define GRID_SOCK_INCLUDED

#include "../protocol.h"
#include "../transport.h"

#include "../aio/ctx.h"
#include "../aio/fsm.h"

#include "../utils/efd.h"
#include "../utils/sem.h"
#include "../utils/clock.h"
#include "../utils/list.h"

struct grid_pipe;

/*  The maximum implemented transport ID. */
#define GRID_MAX_TRANSPORT 4

/*  The socket-internal statistics  */
#define GRID_STAT_MESSAGES_SENT          301
#define GRID_STAT_MESSAGES_RECEIVED      302
#define GRID_STAT_BYTES_SENT             303
#define GRID_STAT_BYTES_RECEIVED         304


struct grid_sock
{
    /*  Socket state machine. */
    struct grid_fsm fsm;
    int state;

    /*  Pointer to the instance of the specific socket type. */
    struct grid_sockbase *sockbase;

    /*  Pointer to the socket type metadata. */
    struct grid_socktype *socktype;

    int flags;

    struct grid_ctx ctx;
    struct grid_efd sndfd;
    struct grid_efd rcvfd;
    struct grid_sem termsem;
    struct grid_sem relesem;

    /*  TODO: This clock can be accessed from different threads. If RDTSC
        is out-of-sync among different CPU cores, this can be a problem. */
    struct grid_clock clock;

    /*  List of all endpoints associated with the socket. */
    struct grid_list eps;

    /*  List of all endpoint being in the process of shutting down. */
    struct grid_list sdeps;

    /*  Next endpoint ID to assign to a new endpoint. */
    int eid;

    /*  Count of active holds against the socket. */
    int holds;

    /*  Socket-level socket options. */
    int linger;
    int sndbuf;
    int rcvbuf;
    int rcvmaxsize;
    int sndtimeo;
    int rcvtimeo;
    int reconnect_ivl;
    int reconnect_ivl_max;

    /*  Endpoint-specific options.  */
    struct grid_ep_options ep_template;

    /*  Transport-specific socket options. */
    struct grid_optset *optsets [GRID_MAX_TRANSPORT];

    struct {

        /*****  The ever-incrementing counters  *****/

        /*  Successfully established grid_connect() connections  */
        uint64_t established_connections;
        /*  Successfully accepted connections  */
        uint64_t accepted_connections;
        /*  Forcedly closed connections  */
        uint64_t dropped_connections;
        /*  Connections closed by peer  */
        uint64_t broken_connections;
        /*  Errors trying to establish active connection  */
        uint64_t connect_errors;
        /*  Errors binding to specified port  */
        uint64_t bind_errors;
        /*  Errors accepting connections at grid_bind()'ed endpoint  */
        uint64_t accept_errors;

        /*  Messages sent  */
        uint64_t messages_sent;
        /*  Messages received  */
        uint64_t messages_received;
        /*  Bytes sent (sum length of data in messages sent)  */
        uint64_t bytes_sent;
        /*  Bytes recevied (sum length of data in messages received)  */
        uint64_t bytes_received;

        /*****  Level-style values *****/

        /*  Number of currently established connections  */
        int current_connections;
        /*  Number of connections currently in progress  */
        int inprogress_connections;
        /*  The currently set priority for sending data  */
        int current_snd_priority;
        /*  Number of endpoints having last_errno set to non-zero value  */
        int current_ep_errors;

    } statistics;

    /*  The socket name for statistics  */
    char socket_name[64];

    /* Win32 Security Attribute */
    void * sec_attr;
    size_t sec_attr_size;
    int outbuffersz;
    int inbuffersz;

};

/*  Initialise the socket. */
int grid_sock_init (struct grid_sock *self, struct grid_socktype *socktype, int fd);

/*  Called by grid_close() to stop activity on the socket.  It doesn't block. */
void grid_sock_stop (struct grid_sock *self);

/*  Called by grid_close() to deallocate the socket. It's a blocking function
    and can return -EINTR. */
int grid_sock_term (struct grid_sock *self);

/*  Called by sockbase when stopping is done. */
void grid_sock_stopped (struct grid_sock *self);

/*  Called by grid_term() to let the socket know about the process shutdown. */
void grid_sock_zombify (struct grid_sock *self);

/*  Returns the AIO context associated with the socket. */
struct grid_ctx *grid_sock_getctx (struct grid_sock *self);

/*  Returns 1 if the specified socket type is a valid peer for this socket,
    0 otherwise. */
int grid_sock_ispeer (struct grid_sock *self, int socktype);

/*  Add new endpoint to the socket. */
int grid_sock_add_ep (struct grid_sock *self, struct grid_transport *transport,
    int bind, const char *addr);

/*  Remove the endpoint with the specified ID from the socket. */
int grid_sock_rm_ep (struct grid_sock *self, int eid);

/*  Send a message to the socket. */
int grid_sock_send (struct grid_sock *self, struct grid_msg *msg, int flags);

/*  Receive a message from the socket. */
int grid_sock_recv (struct grid_sock *self, struct grid_msg *msg, int flags);

/*  Set a socket option. */
int grid_sock_setopt (struct grid_sock *self, int level, int option,
    const void *optval, size_t optvallen);

/*  Retrieve a socket option. This function is to be called from the API. */
int grid_sock_getopt (struct grid_sock *self, int level, int option,
    void *optval, size_t *optvallen);

/*  Retrieve a socket option. This function is to be called from within
    the socket. */
int grid_sock_getopt_inner (struct grid_sock *self, int level, int option,
    void *optval, size_t *optvallen);

/*  Used by pipes. */
int grid_sock_add (struct grid_sock *self, struct grid_pipe *pipe);
void grid_sock_rm (struct grid_sock *self, struct grid_pipe *pipe);

/*  Monitoring callbacks  */
void grid_sock_report_error(struct grid_sock *self, struct grid_ep *ep,  int errnum);
void grid_sock_stat_increment(struct grid_sock *self, int name, int64_t increment);

/*  Holds and releases. */
int grid_sock_hold (struct grid_sock *self);
void grid_sock_rele (struct grid_sock *self);

#endif


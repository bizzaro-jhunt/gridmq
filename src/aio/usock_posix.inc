/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.
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

#include "../utils/alloc.h"
#include "../utils/closefd.h"
#include "../utils/cont.h"
#include "../utils/fast.h"
#include "../utils/err.h"
#include "../utils/attr.h"

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>

#define GRID_USOCK_STATE_IDLE 1
#define GRID_USOCK_STATE_STARTING 2
#define GRID_USOCK_STATE_BEING_ACCEPTED 3
#define GRID_USOCK_STATE_ACCEPTED 4
#define GRID_USOCK_STATE_CONNECTING 5
#define GRID_USOCK_STATE_ACTIVE 6
#define GRID_USOCK_STATE_REMOVING_FD 7
#define GRID_USOCK_STATE_DONE 8
#define GRID_USOCK_STATE_LISTENING 9
#define GRID_USOCK_STATE_ACCEPTING 10
#define GRID_USOCK_STATE_CANCELLING 11
#define GRID_USOCK_STATE_STOPPING 12
#define GRID_USOCK_STATE_STOPPING_ACCEPT 13
#define GRID_USOCK_STATE_ACCEPTING_ERROR 14

#define GRID_USOCK_ACTION_ACCEPT 1
#define GRID_USOCK_ACTION_BEING_ACCEPTED 2
#define GRID_USOCK_ACTION_CANCEL 3
#define GRID_USOCK_ACTION_LISTEN 4
#define GRID_USOCK_ACTION_CONNECT 5
#define GRID_USOCK_ACTION_ACTIVATE 6
#define GRID_USOCK_ACTION_DONE 7
#define GRID_USOCK_ACTION_ERROR 8
#define GRID_USOCK_ACTION_STARTED 9

#define GRID_USOCK_SRC_FD 1
#define GRID_USOCK_SRC_TASK_CONNECTING 2
#define GRID_USOCK_SRC_TASK_CONNECTED 3
#define GRID_USOCK_SRC_TASK_ACCEPT 4
#define GRID_USOCK_SRC_TASK_SEND 5
#define GRID_USOCK_SRC_TASK_RECV 6
#define GRID_USOCK_SRC_TASK_STOP 7

/*  Private functions. */
static void grid_usock_init_from_fd (struct grid_usock *self, int s);
static int grid_usock_send_raw (struct grid_usock *self, struct msghdr *hdr);
static int grid_usock_recv_raw (struct grid_usock *self, void *buf, size_t *len);
static int grid_usock_geterr (struct grid_usock *self);
static void grid_usock_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_usock_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);

void grid_usock_init (struct grid_usock *self, int src, struct grid_fsm *owner)
{
    /*  Initalise the state machine. */
    grid_fsm_init (&self->fsm, grid_usock_handler, grid_usock_shutdown,
        src, self, owner);
    self->state = GRID_USOCK_STATE_IDLE;

    /*  Choose a worker thread to handle this socket. */
    self->worker = grid_fsm_choose_worker (&self->fsm);

    /*  Actual file descriptor will be generated during 'start' step. */
    self->s = -1;
    self->errnum = 0;

    self->in.buf = NULL;
    self->in.len = 0;
    self->in.batch = NULL;
    self->in.batch_len = 0;
    self->in.batch_pos = 0;
    self->in.pfd = NULL;

    memset (&self->out.hdr, 0, sizeof (struct msghdr));

    /*  Initialise tasks for the worker thread. */
    grid_worker_fd_init (&self->wfd, GRID_USOCK_SRC_FD, &self->fsm);
    grid_worker_task_init (&self->task_connecting, GRID_USOCK_SRC_TASK_CONNECTING,
        &self->fsm);
    grid_worker_task_init (&self->task_connected, GRID_USOCK_SRC_TASK_CONNECTED,
        &self->fsm);
    grid_worker_task_init (&self->task_accept, GRID_USOCK_SRC_TASK_ACCEPT,
        &self->fsm);
    grid_worker_task_init (&self->task_send, GRID_USOCK_SRC_TASK_SEND, &self->fsm);
    grid_worker_task_init (&self->task_recv, GRID_USOCK_SRC_TASK_RECV, &self->fsm);
    grid_worker_task_init (&self->task_stop, GRID_USOCK_SRC_TASK_STOP, &self->fsm);

    /*  Intialise events raised by usock. */
    grid_fsm_event_init (&self->event_established);
    grid_fsm_event_init (&self->event_sent);
    grid_fsm_event_init (&self->event_received);
    grid_fsm_event_init (&self->event_error);

    /*  accepting is not going on at the moment. */
    self->asock = NULL;
}

void grid_usock_term (struct grid_usock *self)
{
    grid_assert_state (self, GRID_USOCK_STATE_IDLE);

    if (self->in.batch)
        grid_free (self->in.batch);

    grid_fsm_event_term (&self->event_error);
    grid_fsm_event_term (&self->event_received);
    grid_fsm_event_term (&self->event_sent);
    grid_fsm_event_term (&self->event_established);

    grid_worker_cancel (self->worker, &self->task_recv);

    grid_worker_task_term (&self->task_stop);
    grid_worker_task_term (&self->task_recv);
    grid_worker_task_term (&self->task_send);
    grid_worker_task_term (&self->task_accept);
    grid_worker_task_term (&self->task_connected);
    grid_worker_task_term (&self->task_connecting);
    grid_worker_fd_term (&self->wfd);

    grid_fsm_term (&self->fsm);
}

int grid_usock_isidle (struct grid_usock *self)
{
    return grid_fsm_isidle (&self->fsm);
}

int grid_usock_start (struct grid_usock *self, int domain, int type, int protocol)
{
    int s;

    /*  If the operating system allows to directly open the socket with CLOEXEC
        flag, do so. That way there are no race conditions. */
#ifdef SOCK_CLOEXEC
    type |= SOCK_CLOEXEC;
#endif

    /* Open the underlying socket. */
    s = socket (domain, type, protocol);
    if (grid_slow (s < 0))
       return -errno;

    grid_usock_init_from_fd (self, s);

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);

    return 0;
}

void grid_usock_start_fd (struct grid_usock *self, int fd)
{
    grid_usock_init_from_fd (self, fd);
    grid_fsm_start (&self->fsm);
    grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_STARTED);
}

static void grid_usock_init_from_fd (struct grid_usock *self, int s)
{
    int rc;
    int opt;

    grid_assert (self->state == GRID_USOCK_STATE_IDLE ||
        GRID_USOCK_STATE_BEING_ACCEPTED);

    /*  Store the file descriptor. */
    grid_assert (self->s == -1);
    self->s = s;

    /* Setting FD_CLOEXEC option immediately after socket creation is the
        second best option after using SOCK_CLOEXEC. There is a race condition
        here (if process is forked between socket creation and setting
        the option) but the problem is pretty unlikely to happen. */
#if defined FD_CLOEXEC
    rc = fcntl (self->s, F_SETFD, FD_CLOEXEC);
#if defined GRID_HAVE_OSX
    errno_assert (rc != -1 || errno == EINVAL);
#else
    errno_assert (rc != -1);
#endif
#endif

    /* If applicable, prevent SIGPIPE signal when writing to the connection
        already closed by the peer. */
#ifdef SO_NOSIGPIPE
    opt = 1;
    rc = setsockopt (self->s, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof (opt));
#if defined GRID_HAVE_OSX
    errno_assert (rc == 0 || errno == EINVAL);
#else
    errno_assert (rc == 0);
#endif
#endif

    /* Switch the socket to the non-blocking mode. All underlying sockets
        are always used in the callbackhronous mode. */
    opt = fcntl (self->s, F_GETFL, 0);
    if (opt == -1)
        opt = 0;
    if (!(opt & O_NONBLOCK)) {
        rc = fcntl (self->s, F_SETFL, opt | O_NONBLOCK);
#if defined GRID_HAVE_OSX
        errno_assert (rc != -1 || errno == EINVAL);
#else
        errno_assert (rc != -1);
#endif
    }
}

void grid_usock_stop (struct grid_usock *self)
{
    grid_fsm_stop (&self->fsm);
}

void grid_usock_async_stop (struct grid_usock *self)
{
    grid_worker_execute (self->worker, &self->task_stop);
    grid_fsm_raise (&self->fsm, &self->event_error, GRID_USOCK_SHUTDOWN);
}

void grid_usock_swap_owner (struct grid_usock *self, struct grid_fsm_owner *owner)
{
    grid_fsm_swap_owner (&self->fsm, owner);
}

int grid_usock_setsockopt (struct grid_usock *self, int level, int optname,
    const void *optval, size_t optlen)
{
    int rc;

    /*  The socket can be modified only before it's active. */
    grid_assert (self->state == GRID_USOCK_STATE_STARTING ||
        self->state == GRID_USOCK_STATE_ACCEPTED);

    /*  EINVAL errors are ignored on OSX platform. The reason for that is buggy
        OSX behaviour where setsockopt returns EINVAL if the peer have already
        disconnected. Thus, grid_usock_setsockopt() can succeed on OSX even though
        the option value was invalid, but the peer have already closed the
        connection. This behaviour should be relatively harmless. */
    rc = setsockopt (self->s, level, optname, optval, (socklen_t) optlen);
#if defined GRID_HAVE_OSX
    if (grid_slow (rc != 0 && errno != EINVAL))
        return -errno;
#else
    if (grid_slow (rc != 0))
        return -errno;
#endif

    return 0;
}

int grid_usock_bind (struct grid_usock *self, const struct sockaddr *addr,
    size_t addrlen)
{
    int rc;
    int opt;

    /*  The socket can be bound only before it's connected. */
    grid_assert_state (self, GRID_USOCK_STATE_STARTING);

    /*  Allow re-using the address. */
    opt = 1;
    rc = setsockopt (self->s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof (opt));
    errno_assert (rc == 0);

    rc = bind (self->s, addr, (socklen_t) addrlen);
    if (grid_slow (rc != 0))
        return -errno;

    return 0;
}

int grid_usock_listen (struct grid_usock *self, int backlog)
{
    int rc;

    /*  You can start listening only before the socket is connected. */
    grid_assert_state (self, GRID_USOCK_STATE_STARTING);

    /*  Start listening for incoming connections. */
    rc = listen (self->s, backlog);
    if (grid_slow (rc != 0))
        return -errno;

    /*  Notify the state machine. */
    grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_LISTEN);

    return 0;
}

void grid_usock_accept (struct grid_usock *self, struct grid_usock *listener)
{
    int s;

    /*  Start the actual accepting. */
    if (grid_fsm_isidle(&self->fsm)) {
        grid_fsm_start (&self->fsm);
        grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_BEING_ACCEPTED);
    }
    grid_fsm_action (&listener->fsm, GRID_USOCK_ACTION_ACCEPT);

    /*  Try to accept new connection in synchronous manner. */
#if GRID_HAVE_ACCEPT4
    s = accept4 (listener->s, NULL, NULL, SOCK_CLOEXEC);
#else
    s = accept (listener->s, NULL, NULL);
#endif

    /*  Immediate success. */
    if (grid_fast (s >= 0)) {
        /*  Disassociate the listener socket from the accepted
            socket. Is useful if we restart accepting on ACCEPT_ERROR  */
        listener->asock = NULL;
        self->asock = NULL;

        grid_usock_init_from_fd (self, s);
        grid_fsm_action (&listener->fsm, GRID_USOCK_ACTION_DONE);
        grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_DONE);
        return;
    }

    /*  Detect a failure. Note that in ECONNABORTED case we simply ignore
        the error and wait for next connection in asynchronous manner. */
    errno_assert (errno == EAGAIN || errno == EWOULDBLOCK ||
        errno == ECONNABORTED || errno == ENFILE || errno == EMFILE ||
        errno == ENOBUFS || errno == ENOMEM);

    /*  Pair the two sockets.  They are already paired in case
        previous attempt failed on ACCEPT_ERROR  */
    grid_assert (!self->asock || self->asock == listener);
    self->asock = listener;
    grid_assert (!listener->asock || listener->asock == self);
    listener->asock = self;

    /*  Some errors are just ok to ignore for now.  We also stop repeating
        any errors until next IN_FD event so that we are not in a tight loop
        and allow processing other events in the meantime  */
    if (grid_slow (errno != EAGAIN && errno != EWOULDBLOCK
        && errno != ECONNABORTED && errno != listener->errnum))
    {
        listener->errnum = errno;
        listener->state = GRID_USOCK_STATE_ACCEPTING_ERROR;
        grid_fsm_raise (&listener->fsm,
            &listener->event_error, GRID_USOCK_ACCEPT_ERROR);
        return;
    }

    /*  Ask the worker thread to wait for the new connection. */
    grid_worker_execute (listener->worker, &listener->task_accept);
}

void grid_usock_activate (struct grid_usock *self)
{
    grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_ACTIVATE);
}

void grid_usock_connect (struct grid_usock *self, const struct sockaddr *addr,
    size_t addrlen)
{
    int rc;

    /*  Notify the state machine that we've started connecting. */
    grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_CONNECT);

    /* Do the connect itself. */
    rc = connect (self->s, addr, (socklen_t) addrlen);

    /* Immediate success. */
    if (grid_fast (rc == 0)) {
        grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_DONE);
        return;
    }

    /*  Immediate error. */
    if (grid_slow (errno != EINPROGRESS)) {
        self->errnum = errno;
        grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_ERROR);
        return;
    }

    /*  Start asynchronous connect. */
    grid_worker_execute (self->worker, &self->task_connecting);
}

void grid_usock_send (struct grid_usock *self, const struct grid_iovec *iov,
    int iovcnt)
{
    int rc;
    int i;
    int out;

    /*  Make sure that the socket is actually alive. */
    grid_assert_state (self, GRID_USOCK_STATE_ACTIVE);

    /*  Copy the iovecs to the socket. */
    grid_assert (iovcnt <= GRID_USOCK_MAX_IOVCNT);
    self->out.hdr.msg_iov = self->out.iov;
    out = 0;
    for (i = 0; i != iovcnt; ++i) {
        if (iov [i].iov_len == 0)
            continue;
        self->out.iov [out].iov_base = iov [i].iov_base;
        self->out.iov [out].iov_len = iov [i].iov_len;
        out++;
    }
    self->out.hdr.msg_iovlen = out;

    /*  Try to send the data immediately. */
    rc = grid_usock_send_raw (self, &self->out.hdr);

    /*  Success. */
    if (grid_fast (rc == 0)) {
        grid_fsm_raise (&self->fsm, &self->event_sent, GRID_USOCK_SENT);
        return;
    }

    /*  Errors. */
    if (grid_slow (rc != -EAGAIN)) {
        errnum_assert (rc == -ECONNRESET, -rc);
        grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_ERROR);
        return;
    }

    /*  Ask the worker thread to send the remaining data. */
    grid_worker_execute (self->worker, &self->task_send);
}

void grid_usock_recv (struct grid_usock *self, void *buf, size_t len, int *fd)
{
    int rc;
    size_t nbytes;

    /*  Make sure that the socket is actually alive. */
    grid_assert_state (self, GRID_USOCK_STATE_ACTIVE);

    /*  Try to receive the data immediately. */
    nbytes = len;
    self->in.pfd = fd;
    rc = grid_usock_recv_raw (self, buf, &nbytes);
    if (grid_slow (rc < 0)) {
        errnum_assert (rc == -ECONNRESET, -rc);
        grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_ERROR);
        return;
    }

    /*  Success. */
    if (grid_fast (nbytes == len)) {
        grid_fsm_raise (&self->fsm, &self->event_received, GRID_USOCK_RECEIVED);
        return;
    }

    /*  There are still data to receive in the background. */
    self->in.buf = ((uint8_t*) buf) + nbytes;
    self->in.len = len - nbytes;

    /*  Ask the worker thread to receive the remaining data. */
    grid_worker_execute (self->worker, &self->task_recv);
}

static int grid_internal_tasks (struct grid_usock *usock, int src, int type)
{

/******************************************************************************/
/*  Internal tasks sent from the user thread to the worker thread.            */
/******************************************************************************/
    switch (src) {
    case GRID_USOCK_SRC_TASK_SEND:
        grid_assert (type == GRID_WORKER_TASK_EXECUTE);
        grid_worker_set_out (usock->worker, &usock->wfd);
        return 1;
    case GRID_USOCK_SRC_TASK_RECV:
        grid_assert (type == GRID_WORKER_TASK_EXECUTE);
        grid_worker_set_in (usock->worker, &usock->wfd);
        return 1;
    case GRID_USOCK_SRC_TASK_CONNECTED:
        grid_assert (type == GRID_WORKER_TASK_EXECUTE);
        grid_worker_add_fd (usock->worker, usock->s, &usock->wfd);
        return 1;
    case GRID_USOCK_SRC_TASK_CONNECTING:
        grid_assert (type == GRID_WORKER_TASK_EXECUTE);
        grid_worker_add_fd (usock->worker, usock->s, &usock->wfd);
        grid_worker_set_out (usock->worker, &usock->wfd);
        return 1;
    case GRID_USOCK_SRC_TASK_ACCEPT:
        grid_assert (type == GRID_WORKER_TASK_EXECUTE);
        grid_worker_add_fd (usock->worker, usock->s, &usock->wfd);
        grid_worker_set_in (usock->worker, &usock->wfd);
        return 1;
    }

    return 0;
}

static void grid_usock_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_usock *usock;

    usock = grid_cont (self, struct grid_usock, fsm);

    if (grid_internal_tasks (usock, src, type))
        return;

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {

        /*  Socket in ACCEPTING or CANCELLING state cannot be closed.
            Stop the socket being accepted first. */
        grid_assert (usock->state != GRID_USOCK_STATE_ACCEPTING &&
            usock->state != GRID_USOCK_STATE_CANCELLING);

        usock->errnum = 0;

        /*  Synchronous stop. */
        if (usock->state == GRID_USOCK_STATE_IDLE)
            goto finish3;
        if (usock->state == GRID_USOCK_STATE_DONE)
            goto finish2;
        if (usock->state == GRID_USOCK_STATE_STARTING ||
              usock->state == GRID_USOCK_STATE_ACCEPTED ||
              usock->state == GRID_USOCK_STATE_ACCEPTING_ERROR ||
              usock->state == GRID_USOCK_STATE_LISTENING)
            goto finish1;

        /*  When socket that's being accepted is asked to stop, we have to
            ask the listener socket to stop accepting first. */
        if (usock->state == GRID_USOCK_STATE_BEING_ACCEPTED) {
            grid_fsm_action (&usock->asock->fsm, GRID_USOCK_ACTION_CANCEL);
            usock->state = GRID_USOCK_STATE_STOPPING_ACCEPT;
            return;
        }

        /*  Asynchronous stop. */
        if (usock->state != GRID_USOCK_STATE_REMOVING_FD)
            grid_usock_async_stop (usock);
        usock->state = GRID_USOCK_STATE_STOPPING;
        return;
    }
    if (grid_slow (usock->state == GRID_USOCK_STATE_STOPPING_ACCEPT)) {
        grid_assert (src == GRID_FSM_ACTION && type == GRID_USOCK_ACTION_DONE);
        goto finish2;
    }
    if (grid_slow (usock->state == GRID_USOCK_STATE_STOPPING)) {
        if (src != GRID_USOCK_SRC_TASK_STOP)
            return;
        grid_assert (type == GRID_WORKER_TASK_EXECUTE);
        grid_worker_rm_fd (usock->worker, &usock->wfd);
finish1:
        grid_closefd (usock->s);
        usock->s = -1;
finish2:
        usock->state = GRID_USOCK_STATE_IDLE;
        grid_fsm_stopped (&usock->fsm, GRID_USOCK_STOPPED);
finish3:
        return;
    }

    grid_fsm_bad_state(usock->state, src, type);
}

static void grid_usock_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    int rc;
    struct grid_usock *usock;
    int s;
    size_t sz;
    int sockerr;

    usock = grid_cont (self, struct grid_usock, fsm);

    if(grid_internal_tasks(usock, src, type))
        return;

    switch (usock->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  grid_usock object is initialised, but underlying OS socket is not yet       */
/*  created.                                                                  */
/******************************************************************************/
    case GRID_USOCK_STATE_IDLE:
        switch (src) {
        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                usock->state = GRID_USOCK_STATE_STARTING;
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  STARTING state.                                                           */
/*  Underlying OS socket is created, but it's not yet passed to the worker    */
/*  thread. In this state we can set socket options, local and remote         */
/*  address etc.                                                              */
/******************************************************************************/
    case GRID_USOCK_STATE_STARTING:

        /*  Events from the owner of the usock. */
        switch (src) {
        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_USOCK_ACTION_LISTEN:
                usock->state = GRID_USOCK_STATE_LISTENING;
                return;
            case GRID_USOCK_ACTION_CONNECT:
                usock->state = GRID_USOCK_STATE_CONNECTING;
                return;
            case GRID_USOCK_ACTION_BEING_ACCEPTED:
                usock->state = GRID_USOCK_STATE_BEING_ACCEPTED;
                return;
            case GRID_USOCK_ACTION_STARTED:
                grid_worker_add_fd (usock->worker, usock->s, &usock->wfd);
                usock->state = GRID_USOCK_STATE_ACTIVE;
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  BEING_ACCEPTED state.                                                     */
/*  accept() was called on the usock. Now the socket is waiting for a new     */
/*  connection to arrive.                                                     */
/******************************************************************************/
    case GRID_USOCK_STATE_BEING_ACCEPTED:
        switch (src) {
        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_USOCK_ACTION_DONE:
                usock->state = GRID_USOCK_STATE_ACCEPTED;
                grid_fsm_raise (&usock->fsm, &usock->event_established,
                    GRID_USOCK_ACCEPTED);
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  ACCEPTED state.                                                           */
/*  Connection was accepted, now it can be tuned. Afterwards, it'll move to   */
/*  the active state.                                                         */
/******************************************************************************/
    case GRID_USOCK_STATE_ACCEPTED:
        switch (src) {
        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_USOCK_ACTION_ACTIVATE:
                grid_worker_add_fd (usock->worker, usock->s, &usock->wfd);
                usock->state = GRID_USOCK_STATE_ACTIVE;
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  CONNECTING state.                                                         */
/*  Asynchronous connecting is going on.                                      */
/******************************************************************************/
    case GRID_USOCK_STATE_CONNECTING:
        switch (src) {
        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_USOCK_ACTION_DONE:
                usock->state = GRID_USOCK_STATE_ACTIVE;
                grid_worker_execute (usock->worker, &usock->task_connected);
                grid_fsm_raise (&usock->fsm, &usock->event_established,
                    GRID_USOCK_CONNECTED);
                return;
            case GRID_USOCK_ACTION_ERROR:
                grid_closefd (usock->s);
                usock->s = -1;
                usock->state = GRID_USOCK_STATE_DONE;
                grid_fsm_raise (&usock->fsm, &usock->event_error,
                    GRID_USOCK_ERROR);
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        case GRID_USOCK_SRC_FD:
            switch (type) {
            case GRID_WORKER_FD_OUT:
                grid_worker_reset_out (usock->worker, &usock->wfd);
                usock->state = GRID_USOCK_STATE_ACTIVE;
                sockerr = grid_usock_geterr(usock);
                if (sockerr == 0) {
                    grid_fsm_raise (&usock->fsm, &usock->event_established,
                        GRID_USOCK_CONNECTED);
                } else {
                    usock->errnum = sockerr;
                    grid_worker_rm_fd (usock->worker, &usock->wfd);
                    rc = close (usock->s);
                    errno_assert (rc == 0);
                    usock->s = -1;
                    usock->state = GRID_USOCK_STATE_DONE;
                    grid_fsm_raise (&usock->fsm,
                        &usock->event_error, GRID_USOCK_ERROR);
                }
                return;
            case GRID_WORKER_FD_ERR:
                grid_worker_rm_fd (usock->worker, &usock->wfd);
                grid_closefd (usock->s);
                usock->s = -1;
                usock->state = GRID_USOCK_STATE_DONE;
                grid_fsm_raise (&usock->fsm, &usock->event_error, GRID_USOCK_ERROR);
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  Socket is connected. It can be used for sending and receiving data.       */
/******************************************************************************/
    case GRID_USOCK_STATE_ACTIVE:
        switch (src) {
        case GRID_USOCK_SRC_FD:
            switch (type) {
            case GRID_WORKER_FD_IN:
                sz = usock->in.len;
                rc = grid_usock_recv_raw (usock, usock->in.buf, &sz);
                if (grid_fast (rc == 0)) {
                    usock->in.len -= sz;
                    usock->in.buf += sz;
                    if (!usock->in.len) {
                        grid_worker_reset_in (usock->worker, &usock->wfd);
                        grid_fsm_raise (&usock->fsm, &usock->event_received,
                            GRID_USOCK_RECEIVED);
                    }
                    return;
                }
                errnum_assert (rc == -ECONNRESET, -rc);
                goto error;
            case GRID_WORKER_FD_OUT:
                rc = grid_usock_send_raw (usock, &usock->out.hdr);
                if (grid_fast (rc == 0)) {
                    grid_worker_reset_out (usock->worker, &usock->wfd);
                    grid_fsm_raise (&usock->fsm, &usock->event_sent,
                        GRID_USOCK_SENT);
                    return;
                }
                if (grid_fast (rc == -EAGAIN))
                    return;
                errnum_assert (rc == -ECONNRESET, -rc);
                goto error;
            case GRID_WORKER_FD_ERR:
error:
                grid_worker_rm_fd (usock->worker, &usock->wfd);
                grid_closefd (usock->s);
                usock->s = -1;
                usock->state = GRID_USOCK_STATE_DONE;
                grid_fsm_raise (&usock->fsm, &usock->event_error, GRID_USOCK_ERROR);
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_USOCK_ACTION_ERROR:
                usock->state = GRID_USOCK_STATE_REMOVING_FD;
                grid_usock_async_stop (usock);
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source(usock->state, src, type);
        }

/******************************************************************************/
/*  REMOVING_FD state.                                                        */
/******************************************************************************/
    case GRID_USOCK_STATE_REMOVING_FD:
        switch (src) {
        case GRID_USOCK_SRC_TASK_STOP:
            switch (type) {
            case GRID_WORKER_TASK_EXECUTE:
                grid_worker_rm_fd (usock->worker, &usock->wfd);
                grid_closefd (usock->s);
                usock->s = -1;
                usock->state = GRID_USOCK_STATE_DONE;
                grid_fsm_raise (&usock->fsm, &usock->event_error, GRID_USOCK_ERROR);
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }

        /*  Events from the file descriptor are ignored while it is being
            removed. */
        case GRID_USOCK_SRC_FD:
            return;

        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  DONE state.                                                               */
/*  Socket is closed. The only thing that can be done in this state is        */
/*  stopping the usock.                                                       */
/******************************************************************************/
    case GRID_USOCK_STATE_DONE:
        grid_fsm_bad_source (usock->state, src, type);

/******************************************************************************/
/*  LISTENING state.                                                          */
/*  Socket is listening for new incoming connections, however, user is not    */
/*  accepting a new connection.                                               */
/******************************************************************************/
    case GRID_USOCK_STATE_LISTENING:
        switch (src) {
        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_USOCK_ACTION_ACCEPT:
                usock->state = GRID_USOCK_STATE_ACCEPTING;
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  ACCEPTING state.                                                          */
/*  User is waiting asynchronouslyfor a new inbound connection                */
/*  to be accepted.                                                           */
/******************************************************************************/
    case GRID_USOCK_STATE_ACCEPTING:
        switch (src) {
        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_USOCK_ACTION_DONE:
                usock->state = GRID_USOCK_STATE_LISTENING;
                return;
            case GRID_USOCK_ACTION_CANCEL:
                usock->state = GRID_USOCK_STATE_CANCELLING;
                grid_worker_execute (usock->worker, &usock->task_stop);
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        case GRID_USOCK_SRC_FD:
            switch (type) {
            case GRID_WORKER_FD_IN:

                /*  New connection arrived in asynchronous manner. */
#if GRID_HAVE_ACCEPT4
                s = accept4 (usock->s, NULL, NULL, SOCK_CLOEXEC);
#else
                s = accept (usock->s, NULL, NULL);
#endif

                /*  ECONNABORTED is an valid error. New connection was closed
                    by the peer before we were able to accept it. If it happens
                    do nothing and wait for next incoming connection. */
                if (grid_slow (s < 0 && errno == ECONNABORTED))
                    return;

                /*  Resource allocation errors. It's not clear from POSIX
                    specification whether the new connection is closed in this
                    case or whether it remains in the backlog. In the latter
                    case it would be wise to wait here for a while to prevent
                    busy looping. */
                if (grid_slow (s < 0 && (errno == ENFILE || errno == EMFILE ||
                      errno == ENOBUFS || errno == ENOMEM))) {
                    usock->errnum = errno;
                    usock->state = GRID_USOCK_STATE_ACCEPTING_ERROR;

                    /*  Wait till the user starts accepting once again. */
                    grid_worker_rm_fd (usock->worker, &usock->wfd);

                    grid_fsm_raise (&usock->fsm,
                        &usock->event_error, GRID_USOCK_ACCEPT_ERROR);
                    return;
                }

                /* Any other error is unexpected. */
                errno_assert (s >= 0);

                /*  Initialise the new usock object. */
                grid_usock_init_from_fd (usock->asock, s);
                usock->asock->state = GRID_USOCK_STATE_ACCEPTED;

                /*  Notify the user that connection was accepted. */
                grid_fsm_raise (&usock->asock->fsm,
                    &usock->asock->event_established, GRID_USOCK_ACCEPTED);

                /*  Disassociate the listener socket from the accepted
                    socket. */
                usock->asock->asock = NULL;
                usock->asock = NULL;

                /*  Wait till the user starts accepting once again. */
                grid_worker_rm_fd (usock->worker, &usock->wfd);
                usock->state = GRID_USOCK_STATE_LISTENING;

                return;

            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  ACCEPTING_ERROR state.                                                    */
/*  Waiting the socket to accept the error and restart                        */
/******************************************************************************/
    case GRID_USOCK_STATE_ACCEPTING_ERROR:
        switch (src) {
        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_USOCK_ACTION_ACCEPT:
                usock->state = GRID_USOCK_STATE_ACCEPTING;
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  CANCELLING state.                                                         */
/******************************************************************************/
    case GRID_USOCK_STATE_CANCELLING:
        switch (src) {
        case GRID_USOCK_SRC_TASK_STOP:
            switch (type) {
            case GRID_WORKER_TASK_EXECUTE:
                grid_worker_rm_fd (usock->worker, &usock->wfd);
                usock->state = GRID_USOCK_STATE_LISTENING;

                /*  Notify the accepted socket that it was stopped. */
                grid_fsm_action (&usock->asock->fsm, GRID_USOCK_ACTION_DONE);

                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        case GRID_USOCK_SRC_FD:
            switch (type) {
            case GRID_WORKER_FD_IN:
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/******************************************************************************/
/*  Invalid state                                                             */
/******************************************************************************/
    default:
        grid_fsm_bad_state (usock->state, src, type);
    }
}

static int grid_usock_send_raw (struct grid_usock *self, struct msghdr *hdr)
{
    ssize_t nbytes;

    /*  Try to send the data. */
#if defined MSG_NOSIGNAL
    nbytes = sendmsg (self->s, hdr, MSG_NOSIGNAL);
#else
    nbytes = sendmsg (self->s, hdr, 0);
#endif

    /*  Handle errors. */
    if (grid_slow (nbytes < 0)) {
        if (grid_fast (errno == EAGAIN || errno == EWOULDBLOCK))
            nbytes = 0;
        else {

            /*  If the connection fails, return ECONNRESET. */
            return -ECONNRESET;
        }
    }

    /*  Some bytes were sent. Adjust the iovecs accordingly. */
    while (nbytes) {
        if (nbytes >= (ssize_t)hdr->msg_iov->iov_len) {
            --hdr->msg_iovlen;
            if (!hdr->msg_iovlen) {
                grid_assert (nbytes == (ssize_t)hdr->msg_iov->iov_len);
                return 0;
            }
            nbytes -= hdr->msg_iov->iov_len;
            ++hdr->msg_iov;
        }
        else {
            *((uint8_t**) &(hdr->msg_iov->iov_base)) += nbytes;
            hdr->msg_iov->iov_len -= nbytes;
            return -EAGAIN;
        }
    }

    if (hdr->msg_iovlen > 0)
        return -EAGAIN;

    return 0;
}

static int grid_usock_recv_raw (struct grid_usock *self, void *buf, size_t *len)
{
    size_t sz;
    size_t length;
    ssize_t nbytes;
    struct iovec iov;
    struct msghdr hdr;
    unsigned char ctrl [256];
#if defined GRID_HAVE_MSG_CONTROL
    struct cmsghdr *cmsg;
#endif

    /*  If batch buffer doesn't exist, allocate it. The point of delayed
        deallocation to allow non-receiving sockets, such as TCP listening
        sockets, to do without the batch buffer. */
    if (grid_slow (!self->in.batch)) {
        self->in.batch = grid_alloc (GRID_USOCK_BATCH_SIZE, "AIO batch buffer");
        alloc_assert (self->in.batch);
    }

    /*  Try to satisfy the recv request by data from the batch buffer. */
    length = *len;
    sz = self->in.batch_len - self->in.batch_pos;
    if (sz) {
        if (sz > length)
            sz = length;
        memcpy (buf, self->in.batch + self->in.batch_pos, sz);
        self->in.batch_pos += sz;
        buf = ((char*) buf) + sz;
        length -= sz;
        if (!length)
            return 0;
    }

    /*  If recv request is greater than the batch buffer, get the data directly
        into the place. Otherwise, read data to the batch buffer. */
    if (length > GRID_USOCK_BATCH_SIZE) {
        iov.iov_base = buf;
        iov.iov_len = length;
    }
    else {
        iov.iov_base = self->in.batch;
        iov.iov_len = GRID_USOCK_BATCH_SIZE;
    }
    memset (&hdr, 0, sizeof (hdr));
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
#if defined GRID_HAVE_MSG_CONTROL
    hdr.msg_control = ctrl;
    hdr.msg_controllen = sizeof (ctrl);
#else
    *((int*) ctrl) = -1;
    hdr.msg_accrights = ctrl;
    hdr.msg_accrightslen = sizeof (int);
#endif
    nbytes = recvmsg (self->s, &hdr, 0);

    /*  Handle any possible errors. */
    if (grid_slow (nbytes <= 0)) {

        if (grid_slow (nbytes == 0))
            return -ECONNRESET;

        /*  Zero bytes received. */
        if (grid_fast (errno == EAGAIN || errno == EWOULDBLOCK))
            nbytes = 0;
        else {

            /*  If the peer closes the connection, return ECONNRESET. */
            return -ECONNRESET;
        }
    }

    /*  Extract the associated file descriptor, if any. */
    if (nbytes > 0) {
#if defined GRID_HAVE_MSG_CONTROL
        cmsg = CMSG_FIRSTHDR (&hdr);
        while (cmsg) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                if (self->in.pfd) {
                    *self->in.pfd = *((int*) CMSG_DATA (cmsg));
                    self->in.pfd = NULL;
                }
                else {
                    grid_closefd (*((int*) CMSG_DATA (cmsg)));
                }
                break;
            }
            cmsg = CMSG_NXTHDR (&hdr, cmsg);
        }
#else
        if (hdr.msg_accrightslen > 0) {
            grid_assert (hdr.msg_accrightslen == sizeof (int));
            if (self->in.pfd) {
                *self->in.pfd = *((int*) hdr.msg_accrights);
                self->in.pfd = NULL;
            }
            else {
                grid_closefd (*((int*) hdr.msg_accrights));
            }
        }
#endif
    }

    /*  If the data were received directly into the place we can return
        straight away. */
    if (length > GRID_USOCK_BATCH_SIZE) {
        length -= nbytes;
        *len -= length;
        return 0;
    }

    /*  New data were read to the batch buffer. Copy the requested amount of it
        to the user-supplied buffer. */
    self->in.batch_len = nbytes;
    self->in.batch_pos = 0;
    if (nbytes) {
        sz = nbytes > (ssize_t)length ? length : (size_t)nbytes;
        memcpy (buf, self->in.batch, sz);
        length -= sz;
        self->in.batch_pos += sz;
    }

    *len -= length;
    return 0;
}

static int grid_usock_geterr (struct grid_usock *self)
{
    int rc;
    int opt;
#if defined GRID_HAVE_HPUX
    int optsz;
#else
    socklen_t optsz;
#endif

    opt = 0;
    optsz = sizeof (opt);
    rc = getsockopt (self->s, SOL_SOCKET, SO_ERROR, &opt, &optsz);

    /*  The following should handle both Solaris and UNIXes derived from BSD. */
    if (rc == -1)
        return errno;
    errno_assert (rc == 0);
    grid_assert (optsz == sizeof (opt));
    return opt;
}

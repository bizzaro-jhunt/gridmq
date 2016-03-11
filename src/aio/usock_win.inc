/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.
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

#include "worker.h"

#include "../utils/err.h"
#include "../utils/cont.h"
#include "../utils/alloc.h"

#include <stddef.h>
#include <string.h>
#include <limits.h>

#define GRID_USOCK_STATE_IDLE 1
#define GRID_USOCK_STATE_STARTING 2
#define GRID_USOCK_STATE_BEING_ACCEPTED 3
#define GRID_USOCK_STATE_ACCEPTED 4
#define GRID_USOCK_STATE_CONNECTING 5
#define GRID_USOCK_STATE_ACTIVE 6
#define GRID_USOCK_STATE_CANCELLING_IO 7
#define GRID_USOCK_STATE_DONE 8
#define GRID_USOCK_STATE_LISTENING 9
#define GRID_USOCK_STATE_ACCEPTING 10
#define GRID_USOCK_STATE_CANCELLING 11
#define GRID_USOCK_STATE_STOPPING 12
#define GRID_USOCK_STATE_STOPPING_ACCEPT 13

#define GRID_USOCK_ACTION_ACCEPT 1
#define GRID_USOCK_ACTION_BEING_ACCEPTED 2
#define GRID_USOCK_ACTION_CANCEL 3
#define GRID_USOCK_ACTION_LISTEN 4
#define GRID_USOCK_ACTION_CONNECT 5
#define GRID_USOCK_ACTION_ACTIVATE 6
#define GRID_USOCK_ACTION_DONE 7
#define GRID_USOCK_ACTION_ERROR 8

#define GRID_USOCK_SRC_IN 1
#define GRID_USOCK_SRC_OUT 2

/*  Private functions. */
static void grid_usock_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_usock_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);
static int grid_usock_cancel_io (struct grid_usock *self);
static void grid_usock_create_io_completion (struct grid_usock *self);
DWORD grid_usock_open_pipe (struct grid_usock *self, const char *name);
void grid_usock_accept_pipe (struct grid_usock *self, struct grid_usock *listener);

void grid_usock_init (struct grid_usock *self, int src, struct grid_fsm *owner)
{
    grid_fsm_init (&self->fsm, grid_usock_handler, grid_usock_shutdown,
        src, self, owner);
    self->state = GRID_USOCK_STATE_IDLE;
    self->s = INVALID_SOCKET;
    self->isaccepted = 0;
    grid_worker_op_init (&self->in, GRID_USOCK_SRC_IN, &self->fsm);
    grid_worker_op_init (&self->out, GRID_USOCK_SRC_OUT, &self->fsm);
    self->domain = -1;
    self->type = -1;
    self->protocol = -1;

    /*  Intialise events raised by usock. */
    grid_fsm_event_init (&self->event_established);
    grid_fsm_event_init (&self->event_sent);
    grid_fsm_event_init (&self->event_received);
    grid_fsm_event_init (&self->event_error);

    /*  No accepting is going on at the moment. */
    self->asock = NULL;
    self->ainfo = NULL;

    /* NamedPipe-related stuff. */
    memset (&self->pipename, 0, sizeof (self->pipename));
    self->pipesendbuf = NULL;
    memset (&self->sec_attr, 0, sizeof (SECURITY_ATTRIBUTES));

    /* default size for both in and out buffers is 4096 */
    self->outbuffersz = 4096;
    self->inbuffersz = 4096;
}

void grid_usock_term (struct grid_usock *self)
{
    grid_assert_state (self, GRID_USOCK_STATE_IDLE);

    if (self->ainfo)
        grid_free (self->ainfo);
    if (self->pipesendbuf)
        grid_free (self->pipesendbuf);
    grid_fsm_event_term (&self->event_error);
    grid_fsm_event_term (&self->event_received);
    grid_fsm_event_term (&self->event_sent);
    grid_fsm_event_term (&self->event_established);
    grid_worker_op_term (&self->out);
    grid_worker_op_term (&self->in);
    grid_fsm_term (&self->fsm);
}

int grid_usock_isidle (struct grid_usock *self)
{
    return grid_fsm_isidle (&self->fsm);
}

int grid_usock_start (struct grid_usock *self, int domain, int type, int protocol)
{
    int rc;
#if defined IPV6_V6ONLY
    DWORD only;
#endif
#if defined HANDLE_FLAG_INHERIT
    BOOL brc;
#endif

    /* NamedPipes aren't sockets. They don't need all the socket
       initialisation stuff. */
    if (domain != AF_UNIX) {

        /*  Open the underlying socket. */
        self->s = socket (domain, type, protocol);
        if (self->s == INVALID_SOCKET)
           return -grid_err_wsa_to_posix (WSAGetLastError ());

        /*  Disable inheriting the socket to the child processes. */
#if defined HANDLE_FLAG_INHERIT
        brc = SetHandleInformation (self->p, HANDLE_FLAG_INHERIT, 0);
        win_assert (brc);
#endif

        /*  IPv4 mapping for IPv6 sockets is disabled by default. Switch it on. */
#if defined IPV6_V6ONLY
        if (domain == AF_INET6) {
            only = 0;
            rc = setsockopt (self->s, IPPROTO_IPV6, IPV6_V6ONLY,
                (const char*) &only, sizeof (only));
            wsa_assert (rc != SOCKET_ERROR);
        }
#endif

        /*  Associate the socket with a worker thread/completion port. */
        grid_usock_create_io_completion (self);
    }

    /*  Remember the type of the socket. */
    self->domain = domain;
    self->type = type;
    self->protocol = protocol;

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);

    return 0;
}

void grid_usock_start_fd (struct grid_usock *self, int fd)
{
    grid_assert (0);
}

void grid_usock_stop (struct grid_usock *self)
{
    grid_fsm_stop (&self->fsm);
}

void grid_usock_swap_owner (struct grid_usock *self, struct grid_fsm_owner *owner)
{
    grid_fsm_swap_owner (&self->fsm, owner);
}

int grid_usock_setsockopt (struct grid_usock *self, int level, int optname,
    const void *optval, size_t optlen)
{
    int rc;

    /*  NamedPipes aren't sockets. We can't set socket options on them.
        For now we'll ignore the options. */
    if (self->domain == AF_UNIX)
        return 0;

    /*  The socket can be modified only before it's active. */
    grid_assert (self->state == GRID_USOCK_STATE_STARTING ||
        self->state == GRID_USOCK_STATE_ACCEPTED);

    grid_assert (optlen < INT_MAX);

    rc = setsockopt (self->s, level, optname, (char*) optval, (int) optlen);
    if (grid_slow (rc == SOCKET_ERROR))
        return -grid_err_wsa_to_posix (WSAGetLastError ());

    return 0;
}

int grid_usock_bind (struct grid_usock *self, const struct sockaddr *addr,
    size_t addrlen)
{
    int rc;
    ULONG opt;

    /*  In the case of named pipes, let's save the address
        for the later use. */
    if (self->domain == AF_UNIX) {
        if (addrlen > sizeof (struct sockaddr_un))
            return -EINVAL;
        memcpy (&self->pipename, addr, addrlen);
        return 0;
    }

    /*  You can set socket options only before the socket is connected. */
    grid_assert_state (self, GRID_USOCK_STATE_STARTING);

    /*  On Windows, the bound port can be hijacked
        if SO_EXCLUSIVEADDRUSE is not set. */
    opt = 1;
    rc = setsockopt (self->s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        (const char*) &opt, sizeof (opt));
    wsa_assert (rc != SOCKET_ERROR);

    grid_assert (addrlen < INT_MAX);
    rc = bind (self->s, addr, (int) addrlen);
    if (grid_slow (rc == SOCKET_ERROR))
       return -grid_err_wsa_to_posix (WSAGetLastError ());

    return 0;
}

int grid_usock_listen (struct grid_usock *self, int backlog)
{
    int rc;

    /*  You can start listening only before the socket is connected. */
    grid_assert_state (self, GRID_USOCK_STATE_STARTING);

    /*  Start listening for incoming connections. NamedPipes are already
        created in the listening state, so no need to do anything here. */
    if (self->domain != AF_UNIX) {
        rc = listen (self->s, backlog);
        if (grid_slow (rc == SOCKET_ERROR))
           return -grid_err_wsa_to_posix (WSAGetLastError ());
    }

    /*  Notify the state machine. */
    grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_LISTEN);

    return 0;
}

void grid_usock_accept (struct grid_usock *self, struct grid_usock *listener)
{
    int rc;
    BOOL brc;
    DWORD nbytes;
 
    /* NamedPipes have their own accepting mechanism. */
    if (listener->domain == AF_UNIX) {
        grid_usock_accept_pipe (self, listener);
        return;
    }

    rc = grid_usock_start (self, listener->domain, listener->type,
        listener->protocol);
    /*  TODO: EMFILE can be returned here. */
    errnum_assert (rc == 0, -rc);
    grid_fsm_action (&listener->fsm, GRID_USOCK_ACTION_ACCEPT);
    grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_BEING_ACCEPTED);

    /*  If the memory for accept information is not yet allocated, do so.  */
    if (!listener->ainfo) {
        listener->ainfo = grid_alloc (512, "accept info");
        alloc_assert (listener->ainfo);
    }

    /*  Wait for the incoming connection. */
    memset (&listener->in.olpd, 0, sizeof (listener->in.olpd));
    brc = AcceptEx (listener->s, self->s, listener->ainfo, 0, 256, 256, &nbytes,
        &listener->in.olpd);

    /*  Immediate success. */
    if (grid_fast (brc == TRUE)) {
        grid_fsm_action (&listener->fsm, GRID_USOCK_ACTION_DONE);
        grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_DONE);
        return;
    }

    /*  We don't expect a synchronous failure at this point. */
    wsa_assert (grid_slow (WSAGetLastError () == WSA_IO_PENDING));

    /*  Pair the two sockets. */
    grid_assert (!self->asock);
    self->asock = listener;
    grid_assert (!listener->asock);
    listener->asock = self;

    /*  Asynchronous accept. */
    grid_worker_op_start (&listener->in, 0);
}

void grid_usock_activate (struct grid_usock *self)
{
    grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_ACTIVATE);
}

void grid_usock_connect (struct grid_usock *self, const struct sockaddr *addr,
    size_t addrlen)
{
    BOOL brc;
    const GUID fid = WSAID_CONNECTEX;
    LPFN_CONNECTEX pconnectex;
    DWORD nbytes;
    DWORD winerror;

    /*  Fail if the socket is already connected, closed or such. */
    grid_assert_state (self, GRID_USOCK_STATE_STARTING);

    /*  Notify the state machine that we've started connecting. */
    grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_CONNECT);

	grid_assert(addrlen < INT_MAX);
    memset (&self->out.olpd, 0, sizeof (self->out.olpd));

    if (self->domain == AF_UNIX) {
        winerror = grid_usock_open_pipe (self, ((struct sockaddr_un*) addr)->sun_path);
    }
    else
    {
        /*  Get the pointer to connect function. */
        brc = WSAIoctl(self->s, SIO_GET_EXTENSION_FUNCTION_POINTER,
            (void*)&fid, sizeof(fid), (void*)&pconnectex, sizeof(pconnectex),
            &nbytes, NULL, NULL) == 0;
        wsa_assert(brc == TRUE);
        grid_assert(nbytes == sizeof(pconnectex));

        /*  Connect itself. */
        brc = pconnectex(self->s, (struct sockaddr*) addr, addrlen,
            NULL, 0, NULL, &self->out.olpd);        
        winerror = brc ? ERROR_SUCCESS : WSAGetLastError();
    }

    /*  Immediate success. */
    if (grid_fast (winerror == ERROR_SUCCESS)) {
        grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_DONE);
        return;
    }

    /*  Immediate error. */
    if (grid_slow (winerror != WSA_IO_PENDING)) {
        grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_ERROR);
        return;
    }

    /*  Asynchronous connect. */
    grid_worker_op_start (&self->out, 0);
}

void grid_usock_send (struct grid_usock *self, const struct grid_iovec *iov,
    int iovcnt)
{
    int rc;
    BOOL brc;
    WSABUF wbuf [GRID_USOCK_MAX_IOVCNT];
    int i;
    size_t len;
    size_t idx;
    DWORD error;

    /*  Make sure that the socket is actually alive. */
    grid_assert_state (self, GRID_USOCK_STATE_ACTIVE);

    /*  Create a WinAPI-style iovec. */
    len = 0;
    grid_assert (iovcnt <= GRID_USOCK_MAX_IOVCNT);
    for (i = 0; i != iovcnt; ++i) {
        wbuf [i].buf = (char FAR*) iov [i].iov_base;
        wbuf [i].len = (u_long) iov [i].iov_len;
        len += iov [i].iov_len;
    }

    /*  Start the send operation. */
    memset (&self->out.olpd, 0, sizeof (self->out.olpd));
    if (self->domain == AF_UNIX)
    {
        /* TODO: Do not copy the buffer, find an efficent way to Write 
           multiple buffers that doesn't affect the state machine. */

        grid_assert (!self->pipesendbuf);
        self->pipesendbuf = grid_alloc (len, "named pipe sendbuf");

        idx = 0;
        for (i = 0; i != iovcnt; ++i) {
            memcpy ((char*)(self->pipesendbuf) + idx, iov [i].iov_base, iov [i].iov_len);
            idx += iov [i].iov_len;
        }
        brc = WriteFile (self->p, self->pipesendbuf, len, NULL, &self->out.olpd);
        if (grid_fast (brc || GetLastError() == ERROR_IO_PENDING)) {
            grid_worker_op_start (&self->out, 0);
            return;
        }
        error = GetLastError();
        win_assert (error == ERROR_NO_DATA);
        self->errnum = EINVAL;
        grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_ERROR);
        return;
    }

    rc = WSASend (self->s, wbuf, iovcnt, NULL, 0, &self->out.olpd, NULL);
    if (grid_fast (rc == 0)) {
        grid_worker_op_start (&self->out, 0);
        return;
    }
    error = WSAGetLastError();
    if (grid_fast (error == WSA_IO_PENDING)) {
        grid_worker_op_start (&self->out, 0);
        return;
    }
    wsa_assert (error == WSAECONNABORTED || error == WSAECONNRESET ||
        error == WSAENETDOWN || error == WSAENETRESET ||
        error == WSAENOBUFS || error == WSAEWOULDBLOCK);
    self->errnum = grid_err_wsa_to_posix (error);
    grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_ERROR);
}

void grid_usock_recv (struct grid_usock *self, void *buf, size_t len, int *fd)
{
    int rc;
    BOOL brc;
    WSABUF wbuf;
    DWORD wflags;
    DWORD error;

    /*  Passing file descriptors is not implemented on Windows platform. */
    if (fd)
        *fd = -1;

    /*  Make sure that the socket is actually alive. */
    grid_assert_state (self, GRID_USOCK_STATE_ACTIVE);

    /*  Start the receive operation. */
    wbuf.len = (u_long) len;
    wbuf.buf = (char FAR*) buf;
    wflags = MSG_WAITALL;
    memset (&self->in.olpd, 0, sizeof (self->in.olpd));
    if (self->domain == AF_UNIX) {
        brc = ReadFile(self->p, buf, len, NULL, &self->in.olpd);
        error = brc ? ERROR_SUCCESS : GetLastError();
    }
    else {
        rc = WSARecv (self->s, &wbuf, 1, NULL, &wflags, &self->in.olpd, NULL);
        error = (rc == 0) ? ERROR_SUCCESS : WSAGetLastError ();
    }

    if (grid_fast (error == ERROR_SUCCESS)) {
        grid_worker_op_start (&self->in, 1);
        return;
    }

    if (grid_fast (error == WSA_IO_PENDING)) {
        grid_worker_op_start (&self->in, 1);
        return;
    }

    if (error == WSAECONNABORTED || error == WSAECONNRESET ||
          error == WSAENETDOWN || error == WSAENETRESET ||
          error == WSAETIMEDOUT || error == WSAEWOULDBLOCK ||
          error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_BROKEN_PIPE) {
        grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_ERROR);
        return;
   }

    wsa_assert (0);
}

static void grid_usock_create_io_completion (struct grid_usock *self)
{
    struct grid_worker *worker;
    HANDLE cp;

    /*  Associate the socket with a worker thread/completion port. */
    worker = grid_fsm_choose_worker (&self->fsm);
    cp = CreateIoCompletionPort (
	    self->p,
        grid_worker_getcp(worker),
		(ULONG_PTR) NULL,
		0);
    grid_assert(cp);
}

static void grid_usock_create_pipe (struct grid_usock *self, const char *name)
{
    char fullname [256];
    /*  First, create a fully qualified name for the named pipe. */
    _snprintf(fullname, sizeof (fullname), "\\\\.\\pipe\\%s", name);

    self->p = CreateNamedPipeA (
        (char*) fullname,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
            PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES,
        self->outbuffersz,
        self->inbuffersz,
        0,
        self->sec_attr);

    /* TODO: How to properly handle self->p == INVALID_HANDLE_VALUE? */
    win_assert (self->p != INVALID_HANDLE_VALUE);

    self->isaccepted = 1;
    grid_usock_create_io_completion (self);
}

DWORD grid_usock_open_pipe (struct grid_usock *self, const char *name)
{
    char fullname [256];
    DWORD winerror;
	DWORD mode;
	BOOL brc;

    /*  First, create a fully qualified name for the named pipe. */
    _snprintf(fullname, sizeof (fullname), "\\\\.\\pipe\\%s", name);

    self->p = CreateFileA (
        fullname,
        GENERIC_READ | GENERIC_WRITE,
        0,
        self->sec_attr,
        OPEN_ALWAYS,
        FILE_FLAG_OVERLAPPED,
        NULL);

    if (self->p == INVALID_HANDLE_VALUE)
        return GetLastError ();

    mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;
    brc = SetNamedPipeHandleState (
	    self->p,
		&mode,
		NULL,
		NULL);
	if (!brc) {
        CloseHandle (self->p);
        self->p = INVALID_HANDLE_VALUE;
        return GetLastError ();
    }
    self->isaccepted = 0;
    grid_usock_create_io_completion (self);

    winerror = GetLastError ();
    if (winerror != ERROR_SUCCESS && winerror != ERROR_ALREADY_EXISTS)
        return winerror;

    return ERROR_SUCCESS;
}

void grid_usock_accept_pipe (struct grid_usock *self, struct grid_usock *listener)
{
    int rc;
    BOOL brc;
    DWORD winerror;

    /*  TODO: EMFILE can be returned here. */
    rc = grid_usock_start (self, listener->domain, listener->type,
        listener->protocol);
    errnum_assert(rc == 0, -rc);

    grid_fsm_action(&listener->fsm, GRID_USOCK_ACTION_ACCEPT);
    grid_fsm_action(&self->fsm, GRID_USOCK_ACTION_BEING_ACCEPTED);

    /*  If the memory for accept information is not yet allocated, do so now. */
    if (!listener->ainfo) {
        listener->ainfo = grid_alloc (512, "accept info");
        alloc_assert (listener->ainfo);
    }

    /*  Wait for the incoming connection. */
    memset (&listener->in.olpd, 0, sizeof(listener->in.olpd));
    grid_usock_create_pipe (self, listener->pipename.sun_path);
    brc = ConnectNamedPipe (self->p, (LPOVERLAPPED) &listener->in.olpd);

    /*  TODO: Can this function possibly succeed? */
    grid_assert (brc == 0);
    winerror = GetLastError();

    /*  Immediate success. */
    if (grid_fast (winerror == ERROR_PIPE_CONNECTED)) {
        grid_fsm_action (&listener->fsm, GRID_USOCK_ACTION_DONE);
        grid_fsm_action (&self->fsm, GRID_USOCK_ACTION_DONE);
        return;
    }

    /*  We don't expect a synchronous failure at this point. */
    wsa_assert (grid_slow (winerror == WSA_IO_PENDING));

    /*  Pair the two sockets. */
    grid_assert (!self->asock);
    self->asock = listener;
    grid_assert (!listener->asock);
    listener->asock = self;

    /*  Asynchronous accept. */
    grid_worker_op_start (&listener->in, 0);
}

static void grid_usock_close (struct grid_usock *self)
{
    int rc;
    BOOL brc;

    if (self->domain == AF_UNIX) {
        if (self->p == INVALID_HANDLE_VALUE)
            return;
        if (self->isaccepted)
            DisconnectNamedPipe(self->p);
        brc = CloseHandle (self->p);
        self->p = INVALID_HANDLE_VALUE;
        win_assert (brc);
    }
    else
    {
        rc = closesocket (self->s);
        self->s = INVALID_SOCKET;
        wsa_assert (rc == 0);
    }
}

static void grid_usock_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    struct grid_usock *usock;

    usock = grid_cont (self, struct grid_usock, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {

        /*  Socket in ACCEPTING state cannot be closed.
            Stop the socket being accepted first. */
        grid_assert (usock->state != GRID_USOCK_STATE_ACCEPTING);

        /*  Synchronous stop. */
        if (usock->state == GRID_USOCK_STATE_IDLE)
            goto finish3;
        if (usock->state == GRID_USOCK_STATE_DONE)
            goto finish2;
        if (usock->state == GRID_USOCK_STATE_STARTING ||
              usock->state == GRID_USOCK_STATE_ACCEPTED ||
              usock->state == GRID_USOCK_STATE_LISTENING)
            goto finish1;

        /*  When socket that's being accepted is asked to stop, we have to
            ask the listener socket to stop accepting first. */
        if (usock->state == GRID_USOCK_STATE_BEING_ACCEPTED) {
            grid_fsm_action (&usock->asock->fsm, GRID_USOCK_ACTION_CANCEL);
            usock->state = GRID_USOCK_STATE_STOPPING_ACCEPT;
            return;
        }

        /*  If we were already in the process of cancelling overlapped
            operations, we don't have to do anything. Continue waiting
            till cancelling is finished. */
        if (usock->state == GRID_USOCK_STATE_CANCELLING_IO) {
            usock->state = GRID_USOCK_STATE_STOPPING;
            return;
        }

        /*  Notify our parent that pipe socket is shutting down  */
        grid_fsm_raise (&usock->fsm, &usock->event_error, GRID_USOCK_SHUTDOWN);

        /*  In all remaining states we'll simply cancel all overlapped
            operations. */
        if (grid_usock_cancel_io (usock) == 0)
            goto finish1;
        usock->state = GRID_USOCK_STATE_STOPPING;
        return;
    }
    if (grid_slow (usock->state == GRID_USOCK_STATE_STOPPING_ACCEPT)) {
        grid_assert (src == GRID_FSM_ACTION && type == GRID_USOCK_ACTION_DONE);
        goto finish1;
    }
    if (grid_slow (usock->state == GRID_USOCK_STATE_STOPPING)) {
        if (!grid_worker_op_isidle (&usock->in) ||
              !grid_worker_op_isidle (&usock->out))
            return;
finish1:
        grid_usock_close(usock);
finish2:
        usock->state = GRID_USOCK_STATE_IDLE;
        grid_fsm_stopped (&usock->fsm, GRID_USOCK_STOPPED);
finish3:
        return;
    }

    grid_fsm_bad_state(usock->state, src, type);
}

static void grid_usock_handler (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    struct grid_usock *usock;

    usock = grid_cont (self, struct grid_usock, fsm);

    switch (usock->state) {

/*****************************************************************************/
/*  IDLE state.                                                              */
/*****************************************************************************/
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

/*****************************************************************************/
/*  STARTING state.                                                          */
/*****************************************************************************/
    case GRID_USOCK_STATE_STARTING:
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
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  BEING_ACCEPTED state.                                                    */
/*****************************************************************************/
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

/*****************************************************************************/
/*  ACCEPTED state.                                                          */
/*****************************************************************************/
    case GRID_USOCK_STATE_ACCEPTED:
        switch (src) {
        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_USOCK_ACTION_ACTIVATE:
                usock->state = GRID_USOCK_STATE_ACTIVE;
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  CONNECTING state.                                                        */
/*****************************************************************************/
    case GRID_USOCK_STATE_CONNECTING:
        switch (src) {
        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_USOCK_ACTION_DONE:
                usock->state = GRID_USOCK_STATE_ACTIVE;
                grid_fsm_raise (&usock->fsm, &usock->event_established,
                    GRID_USOCK_CONNECTED);
                return;
            case GRID_USOCK_ACTION_ERROR:
                grid_usock_close(usock);
                usock->state = GRID_USOCK_STATE_DONE;
                grid_fsm_raise (&usock->fsm, &usock->event_error, GRID_USOCK_ERROR);
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        case GRID_USOCK_SRC_OUT:
            switch (type) {
            case GRID_WORKER_OP_DONE:
                usock->state = GRID_USOCK_STATE_ACTIVE;
                grid_fsm_raise (&usock->fsm, &usock->event_established,
                    GRID_USOCK_CONNECTED);
                return;
            case GRID_WORKER_OP_ERROR:
                grid_usock_close(usock);
                usock->state = GRID_USOCK_STATE_DONE;
                grid_fsm_raise (&usock->fsm, &usock->event_error, GRID_USOCK_ERROR);
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  ACTIVE state.                                                            */
/*****************************************************************************/
    case GRID_USOCK_STATE_ACTIVE:
        switch (src) {
        case GRID_USOCK_SRC_IN:
            switch (type) {
            case GRID_WORKER_OP_DONE:
                grid_fsm_raise (&usock->fsm, &usock->event_received,
                    GRID_USOCK_RECEIVED);
                return;
            case GRID_WORKER_OP_ERROR:
                if (grid_usock_cancel_io (usock) == 0) {
                    grid_fsm_raise(&usock->fsm, &usock->event_error,
                        GRID_USOCK_ERROR);
                    grid_usock_close (usock);
                    usock->state = GRID_USOCK_STATE_DONE;
                    return;
                }
                usock->state = GRID_USOCK_STATE_CANCELLING_IO;
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        case GRID_USOCK_SRC_OUT:
            switch (type) {
            case GRID_WORKER_OP_DONE:
                if (usock->pipesendbuf) {
                    grid_free(usock->pipesendbuf);
                    usock->pipesendbuf = NULL;
                }
                grid_fsm_raise (&usock->fsm, &usock->event_sent, GRID_USOCK_SENT);
                return;
            case GRID_WORKER_OP_ERROR:
                if (grid_usock_cancel_io (usock) == 0) {
                    grid_fsm_raise(&usock->fsm, &usock->event_error,
                        GRID_USOCK_ERROR);
                    grid_usock_close(usock);
                    usock->state = GRID_USOCK_STATE_DONE;
                    return;
                }
                usock->state = GRID_USOCK_STATE_CANCELLING_IO;
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_USOCK_ACTION_ERROR:
                if (grid_usock_cancel_io (usock) == 0) {
                    grid_fsm_raise(&usock->fsm, &usock->event_error,
                        GRID_USOCK_SHUTDOWN);
                    grid_usock_close(usock);
                    usock->state = GRID_USOCK_STATE_DONE;
                    return;
                }
                usock->state = GRID_USOCK_STATE_CANCELLING_IO;
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  CANCELLING_IO state.                                                     */
/*****************************************************************************/
    case GRID_USOCK_STATE_CANCELLING_IO:
        switch (src) {
        case GRID_USOCK_SRC_IN:
        case GRID_USOCK_SRC_OUT:
            if (!grid_worker_op_isidle (&usock->in) ||
                  !grid_worker_op_isidle (&usock->out))
                return;
            grid_fsm_raise(&usock->fsm, &usock->event_error, GRID_USOCK_SHUTDOWN);
            grid_usock_close(usock);
            usock->state = GRID_USOCK_STATE_DONE;
            return;
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  DONE state.                                                              */
/*****************************************************************************/
    case GRID_USOCK_STATE_DONE:
        grid_fsm_bad_source (usock->state, src, type);

/*****************************************************************************/
/*  LISTENING state.                                                         */
/*****************************************************************************/
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

/*****************************************************************************/
/*  ACCEPTING state.                                                         */
/*****************************************************************************/
    case GRID_USOCK_STATE_ACCEPTING:
        switch (src) {
        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_USOCK_ACTION_DONE:
                usock->state = GRID_USOCK_STATE_LISTENING;
                return;
            case GRID_USOCK_ACTION_CANCEL:
                if (usock->p == INVALID_HANDLE_VALUE && usock->asock != NULL && usock->domain == AF_UNIX) {
                    usock->p = usock->asock->p;
                    grid_usock_cancel_io (usock);
                    usock->p = INVALID_HANDLE_VALUE;
                }
                else
                {
                    grid_usock_cancel_io(usock);
                }
                usock->state = GRID_USOCK_STATE_CANCELLING;
                return;
            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        case GRID_USOCK_SRC_IN:
            switch (type) {
            case GRID_WORKER_OP_DONE:

                /*  Adjust the new usock object. */
                usock->asock->state = GRID_USOCK_STATE_ACCEPTED;

                /*  Notify the user that connection was accepted. */
                grid_fsm_raise (&usock->asock->fsm,
                    &usock->asock->event_established, GRID_USOCK_ACCEPTED);

                /*  Disassociate the listener socket from the accepted
                    socket. */
                usock->asock->asock = NULL;
                usock->asock = NULL;

                /*  Wait till the user starts accepting once again. */
                usock->state = GRID_USOCK_STATE_LISTENING;

                return;

            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  CANCELLING state.                                                        */
/*****************************************************************************/
    case GRID_USOCK_STATE_CANCELLING:
        switch (src) {
        case GRID_USOCK_SRC_IN:
            switch (type) {
            case GRID_WORKER_OP_DONE:
            case GRID_WORKER_OP_ERROR:

                /*  TODO: The socket being accepted should be closed here. */

                usock->state = GRID_USOCK_STATE_LISTENING;

                /*  Notify the accepted socket that it was stopped. */
                grid_fsm_action (&usock->asock->fsm, GRID_USOCK_ACTION_DONE);

                return;

            default:
                grid_fsm_bad_action (usock->state, src, type);
            }
        default:
            grid_fsm_bad_source (usock->state, src, type);
        }

/*****************************************************************************/
/*  Invalid state.                                                           */
/*****************************************************************************/
    default:
        grid_fsm_bad_state (usock->state, src, type);
    }
}

/*****************************************************************************/
/*  State machine actions.                                                   */
/*****************************************************************************/

/*  Returns 0 if there's nothing to cancel or 1 otherwise. */
static int grid_usock_cancel_io (struct grid_usock *self)
{
    int rc;
    BOOL brc;

    /*  For some reason simple CancelIo doesn't seem to work here.
        We have to use CancelIoEx instead. */
    rc = 0;
    if (!grid_worker_op_isidle (&self->in)) {
        brc = CancelIoEx (self->p, &self->in.olpd);
        win_assert (brc || GetLastError () == ERROR_NOT_FOUND);
        rc = 1;
    }
    if (!grid_worker_op_isidle (&self->out)) {
        brc = CancelIoEx (self->p, &self->out.olpd);
        win_assert (brc || GetLastError () == ERROR_NOT_FOUND);
        rc = 1;
    }

    return rc;
}

/*
    Copyright (c) 2012-2014 Martin Sustrik  All rights reserved.
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

#include "../protocol.h"
#include "../transport.h"

#include "sock.h"
#include "global.h"
#include "ep.h"

#include "../utils/err.h"
#include "../utils/cont.h"
#include "../utils/fast.h"
#include "../utils/alloc.h"
#include "../utils/msg.h"

#include <limits.h>

/*  These bits specify whether individual efds are signalled or not at
    the moment. Storing this information allows us to avoid redundant signalling
    and unsignalling of the efd objects. */
#define GRID_SOCK_FLAG_IN 1
#define GRID_SOCK_FLAG_OUT 2

/*  Possible states of the socket. */
#define GRID_SOCK_STATE_INIT 1
#define GRID_SOCK_STATE_ACTIVE 2
#define GRID_SOCK_STATE_ZOMBIE 3
#define GRID_SOCK_STATE_STOPPING_EPS 4
#define GRID_SOCK_STATE_STOPPING 5
#define GRID_SOCK_STATE_FINI 6

/*  Events sent to the state machine. */
#define GRID_SOCK_ACTION_ZOMBIFY 1
#define GRID_SOCK_ACTION_STOPPED 2

/*  Subordinated source objects. */
#define GRID_SOCK_SRC_EP 1

/*  Private functions. */
static struct grid_optset *grid_sock_optset (struct grid_sock *self, int id);
static int grid_sock_setopt_inner (struct grid_sock *self, int level,
    int option, const void *optval, size_t optvallen);
static void grid_sock_onleave (struct grid_ctx *self);
static void grid_sock_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_sock_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_sock_action_zombify (struct grid_sock *self);

/*  Initialize a socket.  A hold is placed on the initialized socket for
    the caller as well. */
int grid_sock_init (struct grid_sock *self, struct grid_socktype *socktype, int fd)
{
    int rc;
    int i;

    /* Make sure that at least one message direction is supported. */
    grid_assert (!(socktype->flags & GRID_SOCKTYPE_FLAG_NOSEND) ||
        !(socktype->flags & GRID_SOCKTYPE_FLAG_NORECV));

    /*  Create the AIO context for the SP socket. */
    grid_ctx_init (&self->ctx, grid_global_getpool (), grid_sock_onleave);

    /*  Initialise the state machine. */
    grid_fsm_init_root (&self->fsm, grid_sock_handler,
        grid_sock_shutdown, &self->ctx);
    self->state = GRID_SOCK_STATE_INIT;

    /*  Open the GRID_SNDFD and GRID_RCVFD efds. Do so, only if the socket type
        supports send/recv, as appropriate. */
    if (socktype->flags & GRID_SOCKTYPE_FLAG_NOSEND)
        memset (&self->sndfd, 0xcd, sizeof (self->sndfd));
    else {
        rc = grid_efd_init (&self->sndfd);
        if (grid_slow (rc < 0))
            return rc;
    }
    if (socktype->flags & GRID_SOCKTYPE_FLAG_NORECV)
        memset (&self->rcvfd, 0xcd, sizeof (self->rcvfd));
    else {
        rc = grid_efd_init (&self->rcvfd);
        if (grid_slow (rc < 0)) {
            if (!(socktype->flags & GRID_SOCKTYPE_FLAG_NOSEND))
                grid_efd_term (&self->sndfd);
            return rc;
        }
    }
    grid_sem_init (&self->termsem);
    grid_sem_init (&self->relesem);
    if (grid_slow (rc < 0)) {
        if (!(socktype->flags & GRID_SOCKTYPE_FLAG_NORECV))
            grid_efd_term (&self->rcvfd);
        if (!(socktype->flags & GRID_SOCKTYPE_FLAG_NOSEND))
            grid_efd_term (&self->sndfd);
        return rc;
    }

    self->holds = 1;   /*  Callers hold. */
    self->flags = 0;
    grid_clock_init (&self->clock);
    grid_list_init (&self->eps);
    grid_list_init (&self->sdeps);
    self->eid = 1;

    /*  Default values for GRID_SOL_SOCKET options. */
    self->linger = 1000;
    self->sndbuf = 128 * 1024;
    self->rcvbuf = 128 * 1024;
    self->rcvmaxsize = 1024 * 1024;
    self->sndtimeo = -1;
    self->rcvtimeo = -1;
    self->reconnect_ivl = 100;
    self->reconnect_ivl_max = 0;
    self->ep_template.sndprio = 8;
    self->ep_template.rcvprio = 8;
    self->ep_template.ipv4only = 1;

    /* Initialize statistic entries */
    self->statistics.established_connections = 0;
    self->statistics.accepted_connections = 0;
    self->statistics.dropped_connections = 0;
    self->statistics.broken_connections = 0;
    self->statistics.connect_errors = 0;
    self->statistics.bind_errors = 0;
    self->statistics.accept_errors = 0;

    self->statistics.messages_sent = 0;
    self->statistics.messages_received = 0;
    self->statistics.bytes_sent = 0;
    self->statistics.bytes_received = 0;

    self->statistics.current_connections = 0;
    self->statistics.inprogress_connections = 0;
    self->statistics.current_snd_priority = 0;
    self->statistics.current_ep_errors = 0;

    /*  Should be pretty much enough space for just the number  */
    sprintf(self->socket_name, "%d", fd);

    /* Security attribute */
    self->sec_attr = NULL;
    self->sec_attr_size = 0;
    self->inbuffersz = 4096;
    self->outbuffersz = 4096;

    /*  The transport-specific options are not initialised immediately,
        rather, they are allocated later on when needed. */
    for (i = 0; i != GRID_MAX_TRANSPORT; ++i)
        self->optsets [i] = NULL;

    /*  Create the specific socket type itself. */
    rc = socktype->create ((void*) self, &self->sockbase);
    errnum_assert (rc == 0, -rc);
    self->socktype = socktype;

    /*  Launch the state machine. */
    grid_ctx_enter (&self->ctx);
    grid_fsm_start (&self->fsm);
    grid_ctx_leave (&self->ctx);

    return 0;
}

void grid_sock_stopped (struct grid_sock *self)
{
    /*  TODO: Do the following in a more sane way. */
    self->fsm.stopped.fsm = &self->fsm;
    self->fsm.stopped.src = GRID_FSM_ACTION;
    self->fsm.stopped.srcptr = NULL;
    self->fsm.stopped.type = GRID_SOCK_ACTION_STOPPED;
    grid_ctx_raise (self->fsm.ctx, &self->fsm.stopped);
}

void grid_sock_zombify (struct grid_sock *self)
{
    grid_ctx_enter (&self->ctx);
    grid_fsm_action (&self->fsm, GRID_SOCK_ACTION_ZOMBIFY);
    grid_ctx_leave (&self->ctx);
}

/*  Stop the socket.  This will prevent new calls from aquiring a
    hold on the socket, cause endpoints to shut down, and wake any
    threads waiting to recv or send data. */
void grid_sock_stop (struct grid_sock *self)
{
    grid_ctx_enter (&self->ctx);
    grid_fsm_stop (&self->fsm);
    grid_ctx_leave (&self->ctx);
}

int grid_sock_term (struct grid_sock *self)
{
    int rc;
    int i;

    /*  NOTE: grid_sock_stop must have already been called. */

    /*  Some endpoints may still be alive.  Here we are going to wait
        till they are all closed.  This loop is not interruptible, because
        making it so would leave a partially cleaned up socket, and we don't
        have a way to defer resource deallocation. */
    for (;;) {
        rc = grid_sem_wait (&self->termsem);
        if (grid_slow (rc == -EINTR))
            continue;
        errnum_assert (rc == 0, -rc);
        break;
    }

    /*  Also, wait for all holds on the socket to be released.  */
    for (;;) {
        rc = grid_sem_wait (&self->relesem);
        if (grid_slow (rc == -EINTR))
            continue;
        errnum_assert (rc == 0, -rc);
        break;
    }

    /*  Threads that posted the semaphore(s) can still have the ctx locked
        for a short while. By simply entering the context and exiting it
        immediately we can be sure that any such threads have already
        exited the context. */
    grid_ctx_enter (&self->ctx);
    grid_ctx_leave (&self->ctx);

    /*  At this point, we can be reasonably certain that no other thread
        has any references to the socket. */

    grid_fsm_stopped_noevent (&self->fsm);
    grid_fsm_term (&self->fsm);
    grid_sem_term (&self->termsem);
    grid_list_term (&self->sdeps);
    grid_list_term (&self->eps);
    grid_clock_term (&self->clock);
    grid_ctx_term (&self->ctx);

    /*  Destroy any optsets associated with the socket. */
    for (i = 0; i != GRID_MAX_TRANSPORT; ++i)
        if (self->optsets [i])
            self->optsets [i]->vfptr->destroy (self->optsets [i]);

    return 0;
}

struct grid_ctx *grid_sock_getctx (struct grid_sock *self)
{
    return &self->ctx;
}

int grid_sock_ispeer (struct grid_sock *self, int socktype)
{
    /*  If the peer implements a different SP protocol it is not a valid peer.
        Checking it here ensures that even if faulty protocol implementation
        allows for cross-protocol communication, it will never happen
        in practice. */
    if ((self->socktype->protocol & 0xfff0) != (socktype  & 0xfff0))
        return 0;

    /*  As long as the peer speaks the same protocol, socket type itself
        decides which socket types are to be accepted. */
    return self->socktype->ispeer (socktype);
}

int grid_sock_setopt (struct grid_sock *self, int level, int option,
    const void *optval, size_t optvallen)
{
    int rc;

    grid_ctx_enter (&self->ctx);
    if (grid_slow (self->state == GRID_SOCK_STATE_ZOMBIE)) {
        grid_ctx_leave (&self->ctx);
        return -ETERM;
    }
    rc = grid_sock_setopt_inner (self, level, option, optval, optvallen);
    grid_ctx_leave (&self->ctx);

    return rc;
}

static int grid_sock_setopt_inner (struct grid_sock *self, int level,
    int option, const void *optval, size_t optvallen)
{
    struct grid_optset *optset;
    int val;
    int *dst;

    /*  Protocol-specific socket options. */
    if (level > GRID_SOL_SOCKET)
        return self->sockbase->vfptr->setopt (self->sockbase, level, option,
            optval, optvallen);

    /*  Transport-specific options. */
    if (level < GRID_SOL_SOCKET) {
        optset = grid_sock_optset (self, level);
        if (!optset)
            return -ENOPROTOOPT;
        return optset->vfptr->setopt (optset, option, optval, optvallen);
    }

    /*  Special-casing socket name for now as it's the only string option  */
    if (level == GRID_SOL_SOCKET && option == GRID_SOCKET_NAME) {
        if (optvallen > 63)
            return -EINVAL;
        memcpy (self->socket_name, optval, optvallen);
        self->socket_name [optvallen] = 0;
        return 0;
    }

    /*  At this point we assume that all options are of type int. */
    if (optvallen != sizeof (int))
        return -EINVAL;
    val = *(int*) optval;

    /*  Generic socket-level options. */
    if (level == GRID_SOL_SOCKET) {
        switch (option) {
        case GRID_LINGER:
            dst = &self->linger;
            break;
        case GRID_SNDBUF:
            if (grid_slow (val <= 0))
                return -EINVAL;
            dst = &self->sndbuf;
            break;
        case GRID_RCVBUF:
            if (grid_slow (val <= 0))
                return -EINVAL;
            dst = &self->rcvbuf;
            break;
        case GRID_RCVMAXSIZE:
            if (grid_slow (val < -1))
                return -EINVAL;
            dst = &self->rcvmaxsize;
            break;
        case GRID_SNDTIMEO:
            dst = &self->sndtimeo;
            break;
        case GRID_RCVTIMEO:
            dst = &self->rcvtimeo;
            break;
        case GRID_RECONNECT_IVL:
            if (grid_slow (val < 0))
                return -EINVAL;
            dst = &self->reconnect_ivl;
            break;
        case GRID_RECONNECT_IVL_MAX:
            if (grid_slow (val < 0))
                return -EINVAL;
            dst = &self->reconnect_ivl_max;
            break;
        case GRID_SNDPRIO:
            if (grid_slow (val < 1 || val > 16))
                return -EINVAL;
            dst = &self->ep_template.sndprio;
            break;
        case GRID_RCVPRIO:
            if (grid_slow (val < 1 || val > 16))
                return -EINVAL;
            dst = &self->ep_template.rcvprio;
            break;
        case GRID_IPV4ONLY:
            if (grid_slow (val != 0 && val != 1))
                return -EINVAL;
            dst = &self->ep_template.ipv4only;
            break;
        default:
            return -ENOPROTOOPT;
        }
        *dst = val;

        return 0;
    }

    grid_assert (0);
}

int grid_sock_getopt (struct grid_sock *self, int level, int option,
    void *optval, size_t *optvallen)
{
    int rc;

    grid_ctx_enter (&self->ctx);
    if (grid_slow (self->state == GRID_SOCK_STATE_ZOMBIE)) {
        grid_ctx_leave (&self->ctx);
        return -ETERM;
    }
    rc = grid_sock_getopt_inner (self, level, option, optval, optvallen);
    grid_ctx_leave (&self->ctx);

    return rc;
}

int grid_sock_getopt_inner (struct grid_sock *self, int level,
    int option, void *optval, size_t *optvallen)
{
    int rc;
    struct grid_optset *optset;
    int intval;
    grid_fd fd;

    /*  Generic socket-level options. */
    if (level == GRID_SOL_SOCKET) {
        switch (option) {
        case GRID_DOMAIN:
            intval = self->socktype->domain;
            break;
        case GRID_PROTOCOL:
            intval = self->socktype->protocol;
            break;
        case GRID_LINGER:
            intval = self->linger;
            break;
        case GRID_SNDBUF:
            intval = self->sndbuf;
            break;
        case GRID_RCVBUF:
            intval = self->rcvbuf;
            break;
        case GRID_RCVMAXSIZE:
            intval = self->rcvmaxsize;
            break;
        case GRID_SNDTIMEO:
            intval = self->sndtimeo;
            break;
        case GRID_RCVTIMEO:
            intval = self->rcvtimeo;
            break;
        case GRID_RECONNECT_IVL:
            intval = self->reconnect_ivl;
            break;
        case GRID_RECONNECT_IVL_MAX:
            intval = self->reconnect_ivl_max;
            break;
        case GRID_SNDPRIO:
            intval = self->ep_template.sndprio;
            break;
        case GRID_RCVPRIO:
            intval = self->ep_template.rcvprio;
            break;
        case GRID_IPV4ONLY:
            intval = self->ep_template.ipv4only;
            break;
        case GRID_SNDFD:
            if (self->socktype->flags & GRID_SOCKTYPE_FLAG_NOSEND)
                return -ENOPROTOOPT;
            fd = grid_efd_getfd (&self->sndfd);
            memcpy (optval, &fd,
                *optvallen < sizeof (grid_fd) ? *optvallen : sizeof (grid_fd));
            *optvallen = sizeof (grid_fd);
            return 0;
        case GRID_RCVFD:
            if (self->socktype->flags & GRID_SOCKTYPE_FLAG_NORECV)
                return -ENOPROTOOPT;
            fd = grid_efd_getfd (&self->rcvfd);
            memcpy (optval, &fd,
                *optvallen < sizeof (grid_fd) ? *optvallen : sizeof (grid_fd));
            *optvallen = sizeof (grid_fd);
            return 0;
        case GRID_SOCKET_NAME:
            strncpy (optval, self->socket_name, *optvallen);
            *optvallen = strlen(self->socket_name);
            return 0;
        default:
            return -ENOPROTOOPT;
        }

        memcpy (optval, &intval,
            *optvallen < sizeof (int) ? *optvallen : sizeof (int));
        *optvallen = sizeof (int);

        return 0;
    }

    /*  Protocol-specific socket options. */
    if (level > GRID_SOL_SOCKET)
        return rc = self->sockbase->vfptr->getopt (self->sockbase,
            level, option, optval, optvallen);

    /*  Transport-specific options. */
    if (level < GRID_SOL_SOCKET) {
        optset = grid_sock_optset (self, level);
        if (!optset)
            return -ENOPROTOOPT;
        return optset->vfptr->getopt (optset, option, optval, optvallen);
    }

    grid_assert (0);
}

int grid_sock_add_ep (struct grid_sock *self, struct grid_transport *transport,
    int bind, const char *addr)
{
    int rc;
    struct grid_ep *ep;
    int eid;

    grid_ctx_enter (&self->ctx);

    /*  Instantiate the endpoint. */
    ep = grid_alloc (sizeof (struct grid_ep), "endpoint");
    rc = grid_ep_init (ep, GRID_SOCK_SRC_EP, self, self->eid, transport,
        bind, addr);
    if (grid_slow (rc < 0)) {
        grid_free (ep);
        grid_ctx_leave (&self->ctx);
        return rc;
    }
    grid_ep_start (ep);

    /*  Increase the endpoint ID for the next endpoint. */
    eid = self->eid;
    ++self->eid;

    /*  Add it to the list of active endpoints. */
    grid_list_insert (&self->eps, &ep->item, grid_list_end (&self->eps));

    grid_ctx_leave (&self->ctx);

    return eid;
}

int grid_sock_rm_ep (struct grid_sock *self, int eid)
{
    struct grid_list_item *it;
    struct grid_ep *ep;

    grid_ctx_enter (&self->ctx);

    /*  Find the specified enpoint. */
    ep = NULL;
    for (it = grid_list_begin (&self->eps);
          it != grid_list_end (&self->eps);
          it = grid_list_next (&self->eps, it)) {
        ep = grid_cont (it, struct grid_ep, item);
        if (ep->eid == eid)
            break;
        ep = NULL;
    }

    /*  The endpoint doesn't exist. */
    if (!ep) {
        grid_ctx_leave (&self->ctx);
        return -EINVAL;
    }

    /*  Move the endpoint from the list of active endpoints to the list
        of shutting down endpoints. */
    grid_list_erase (&self->eps, &ep->item);
    grid_list_insert (&self->sdeps, &ep->item, grid_list_end (&self->sdeps));

    /*  Ask the endpoint to stop. Actual terminatation may be delayed
        by the transport. */
    grid_ep_stop (ep);

    grid_ctx_leave (&self->ctx);

    return 0;
}

int grid_sock_send (struct grid_sock *self, struct grid_msg *msg, int flags)
{
    int rc;
    uint64_t deadline;
    uint64_t now;
    int timeout;

    /*  Some sockets types cannot be used for sending messages. */
    if (grid_slow (self->socktype->flags & GRID_SOCKTYPE_FLAG_NOSEND))
        return -ENOTSUP;

    grid_ctx_enter (&self->ctx);

    /*  Compute the deadline for SNDTIMEO timer. */
    if (self->sndtimeo < 0) {
        deadline = -1;
        timeout = -1;
    }
    else {
        deadline = grid_clock_now (&self->clock) + self->sndtimeo;
        timeout = self->sndtimeo;
    }

    while (1) {

        switch (self->state) {
        case GRID_SOCK_STATE_ACTIVE:
        case GRID_SOCK_STATE_INIT:
             break;

        case GRID_SOCK_STATE_ZOMBIE:
            /*  If grid_term() was already called, return ETERM. */
            grid_ctx_leave (&self->ctx);
            return -ETERM;

        case GRID_SOCK_STATE_STOPPING_EPS:
        case GRID_SOCK_STATE_STOPPING:
        case GRID_SOCK_STATE_FINI:
            /*  Socket closed or closing.  Should we return something
                else here; recvmsg(2) for example returns no data in
                this case, like read(2).  The use of indexed file
                descriptors is further problematic, as an FD can be reused
                leading to situations where technically the outstanding
                operation should refer to some other socket entirely.  */
            grid_ctx_leave (&self->ctx);
            return -EBADF;
        }

        /*  Try to send the message in a non-blocking way. */
        rc = self->sockbase->vfptr->send (self->sockbase, msg);
        if (grid_fast (rc == 0)) {
            grid_ctx_leave (&self->ctx);
            return 0;
        }
        grid_assert (rc < 0);

        /*  Any unexpected error is forwarded to the caller. */
        if (grid_slow (rc != -EAGAIN)) {
            grid_ctx_leave (&self->ctx);
            return rc;
        }

        /*  If the message cannot be sent at the moment and the send call
            is non-blocking, return immediately. */
        if (grid_fast (flags & GRID_DONTWAIT)) {
            grid_ctx_leave (&self->ctx);
            return -EAGAIN;
        }

        /*  With blocking send, wait while there are new pipes available
            for sending. */
        grid_ctx_leave (&self->ctx);
        rc = grid_efd_wait (&self->sndfd, timeout);
        if (grid_slow (rc == -ETIMEDOUT))
            return -ETIMEDOUT;
        if (grid_slow (rc == -EINTR))
            return -EINTR;
        if (grid_slow (rc == -EBADF))
            return -EBADF;
        errnum_assert (rc == 0, rc);
        grid_ctx_enter (&self->ctx);
        /*
         *  Double check if pipes are still available for sending
         */
        if (!grid_efd_wait (&self->sndfd, 0)) {
            self->flags |= GRID_SOCK_FLAG_OUT;
        }

        /*  If needed, re-compute the timeout to reflect the time that have
            already elapsed. */
        if (self->sndtimeo >= 0) {
            now = grid_clock_now (&self->clock);
            timeout = (int) (now > deadline ? 0 : deadline - now);
        }
    }
}

int grid_sock_recv (struct grid_sock *self, struct grid_msg *msg, int flags)
{
    int rc;
    uint64_t deadline;
    uint64_t now;
    int timeout;

    /*  Some sockets types cannot be used for receiving messages. */
    if (grid_slow (self->socktype->flags & GRID_SOCKTYPE_FLAG_NORECV))
        return -ENOTSUP;

    grid_ctx_enter (&self->ctx);

    /*  Compute the deadline for RCVTIMEO timer. */
    if (self->rcvtimeo < 0) {
        deadline = -1;
        timeout = -1;
    }
    else {
        deadline = grid_clock_now (&self->clock) + self->rcvtimeo;
        timeout = self->rcvtimeo;
    }

    while (1) {

        switch (self->state) {
        case GRID_SOCK_STATE_ACTIVE:
        case GRID_SOCK_STATE_INIT:
             break;

        case GRID_SOCK_STATE_ZOMBIE:
            /*  If grid_term() was already called, return ETERM. */
            grid_ctx_leave (&self->ctx);
            return -ETERM;

        case GRID_SOCK_STATE_STOPPING_EPS:
        case GRID_SOCK_STATE_STOPPING:
        case GRID_SOCK_STATE_FINI:
            /*  Socket closed or closing.  Should we return something
                else here; recvmsg(2) for example returns no data in
                this case, like read(2).  The use of indexed file
                descriptors is further problematic, as an FD can be reused
                leading to situations where technically the outstanding
                operation should refer to some other socket entirely.  */
            grid_ctx_leave (&self->ctx);
            return -EBADF;
        }

        /*  Try to receive the message in a non-blocking way. */
        rc = self->sockbase->vfptr->recv (self->sockbase, msg);
        if (grid_fast (rc == 0)) {
            grid_ctx_leave (&self->ctx);
            return 0;
        }
        grid_assert (rc < 0);

        /*  Any unexpected error is forwarded to the caller. */
        if (grid_slow (rc != -EAGAIN)) {
            grid_ctx_leave (&self->ctx);
            return rc;
        }

        /*  If the message cannot be received at the moment and the recv call
            is non-blocking, return immediately. */
        if (grid_fast (flags & GRID_DONTWAIT)) {
            grid_ctx_leave (&self->ctx);
            return -EAGAIN;
        }

        /*  With blocking recv, wait while there are new pipes available
            for receiving. */
        grid_ctx_leave (&self->ctx);
        rc = grid_efd_wait (&self->rcvfd, timeout);
        if (grid_slow (rc == -ETIMEDOUT))
            return -ETIMEDOUT;
        if (grid_slow (rc == -EINTR))
            return -EINTR;
        if (grid_slow (rc == -EBADF))
            return -EBADF;
        errnum_assert (rc == 0, rc);
        grid_ctx_enter (&self->ctx);
        /*
         *  Double check if pipes are still available for receiving
         */
        if (!grid_efd_wait (&self->rcvfd, 0)) {
            self->flags |= GRID_SOCK_FLAG_IN;
        }

        /*  If needed, re-compute the timeout to reflect the time that have
            already elapsed. */
        if (self->rcvtimeo >= 0) {
            now = grid_clock_now (&self->clock);
            timeout = (int) (now > deadline ? 0 : deadline - now);
        }
    }
}

int grid_sock_add (struct grid_sock *self, struct grid_pipe *pipe)
{
    int rc;

    rc = self->sockbase->vfptr->add (self->sockbase, pipe);
    if (grid_slow (rc >= 0)) {
        grid_sock_stat_increment (self, GRID_STAT_CURRENT_CONNECTIONS, 1);
    }
    return rc;
}

void grid_sock_rm (struct grid_sock *self, struct grid_pipe *pipe)
{
    self->sockbase->vfptr->rm (self->sockbase, pipe);
    grid_sock_stat_increment (self, GRID_STAT_CURRENT_CONNECTIONS, -1);
}

static void grid_sock_onleave (struct grid_ctx *self)
{
    struct grid_sock *sock;
    int events;

    sock = grid_cont (self, struct grid_sock, ctx);

    /*  If grid_close() was already called there's no point in adjusting the
        snd/rcv file descriptors. */
    if (grid_slow (sock->state != GRID_SOCK_STATE_ACTIVE))
        return;

    /*  Check whether socket is readable and/or writable at the moment. */
    events = sock->sockbase->vfptr->events (sock->sockbase);
    errnum_assert (events >= 0, -events);

    /*  Signal/unsignal IN as needed. */
    if (!(sock->socktype->flags & GRID_SOCKTYPE_FLAG_NORECV)) {
        if (events & GRID_SOCKBASE_EVENT_IN) {
            if (!(sock->flags & GRID_SOCK_FLAG_IN)) {
                sock->flags |= GRID_SOCK_FLAG_IN;
                grid_efd_signal (&sock->rcvfd);
            }
        }
        else {
            if (sock->flags & GRID_SOCK_FLAG_IN) {
                sock->flags &= ~GRID_SOCK_FLAG_IN;
                grid_efd_unsignal (&sock->rcvfd);
            }
        }
    }

    /*  Signal/unsignal OUT as needed. */
    if (!(sock->socktype->flags & GRID_SOCKTYPE_FLAG_NOSEND)) {
        if (events & GRID_SOCKBASE_EVENT_OUT) {
            if (!(sock->flags & GRID_SOCK_FLAG_OUT)) {
                sock->flags |= GRID_SOCK_FLAG_OUT;
                grid_efd_signal (&sock->sndfd);
            }
        }
        else {
            if (sock->flags & GRID_SOCK_FLAG_OUT) {
                sock->flags &= ~GRID_SOCK_FLAG_OUT;
                grid_efd_unsignal (&sock->sndfd);
            }
        }
    }
}

static struct grid_optset *grid_sock_optset (struct grid_sock *self, int id)
{
    int index;
    struct grid_transport *tp;

    /*  Transport IDs are negative and start from -1. */
    index = (-id) - 1;

    /*  Check for invalid indices. */
    if (grid_slow (index < 0 || index >= GRID_MAX_TRANSPORT))
        return NULL;

    /*  If the option set already exists return it. */
    if (grid_fast (self->optsets [index] != NULL))
        return self->optsets [index];

    /*  If the option set doesn't exist yet, create it. */
    tp = grid_global_transport (id);
    if (grid_slow (!tp))
        return NULL;
    if (grid_slow (!tp->optset))
        return NULL;
    self->optsets [index] = tp->optset ();

    return self->optsets [index];
}

static void grid_sock_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    struct grid_sock *sock;
    struct grid_list_item *it;
    struct grid_ep *ep;

    sock = grid_cont (self, struct grid_sock, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        grid_assert (sock->state == GRID_SOCK_STATE_ACTIVE ||
            sock->state == GRID_SOCK_STATE_ZOMBIE);

        /*  Close sndfd and rcvfd. This should make any current
            select/poll using SNDFD and/or RCVFD exit. */
        if (!(sock->socktype->flags & GRID_SOCKTYPE_FLAG_NORECV)) {
            grid_efd_stop (&sock->rcvfd);
        }
        if (!(sock->socktype->flags & GRID_SOCKTYPE_FLAG_NOSEND)) {
            grid_efd_stop (&sock->sndfd);
        }

        /*  Ask all the associated endpoints to stop. */
        it = grid_list_begin (&sock->eps);
        while (it != grid_list_end (&sock->eps)) {
            ep = grid_cont (it, struct grid_ep, item);
            it = grid_list_next (&sock->eps, it);
            grid_list_erase (&sock->eps, &ep->item);
            grid_list_insert (&sock->sdeps, &ep->item,
                grid_list_end (&sock->sdeps));
            grid_ep_stop (ep);

        }
        sock->state = GRID_SOCK_STATE_STOPPING_EPS;
        goto finish2;
    }
    if (grid_slow (sock->state == GRID_SOCK_STATE_STOPPING_EPS)) {

        if (!(src == GRID_SOCK_SRC_EP && type == GRID_EP_STOPPED)) {
            /*  If we got here waiting for EPs to teardown, but src is
                not an EP, then it isn't safe for us to do anything,
                because we just need to wait for the EPs to finish
                up their thing.  Just bail. */
            return;
        }
        /*  Endpoint is stopped. Now we can safely deallocate it. */
        ep = (struct grid_ep*) srcptr;
        grid_list_erase (&sock->sdeps, &ep->item);
        grid_ep_term (ep);
        grid_free (ep);

finish2:
        /*  If all the endpoints are deallocated, we can start stopping
            protocol-specific part of the socket. If there' no stop function
            we can consider it stopped straight away. */
        if (!grid_list_empty (&sock->sdeps))
            return;
        grid_assert (grid_list_empty (&sock->eps));
        sock->state = GRID_SOCK_STATE_STOPPING;
        if (!sock->sockbase->vfptr->stop)
            goto finish1;
        sock->sockbase->vfptr->stop (sock->sockbase);
        return;
    }
    if (grid_slow (sock->state == GRID_SOCK_STATE_STOPPING)) {

        /*  We get here when the deallocation of the socket was delayed by the
            specific socket type. */
        grid_assert (src == GRID_FSM_ACTION && type == GRID_SOCK_ACTION_STOPPED);

finish1:
        /*  Protocol-specific part of the socket is stopped.
            We can safely deallocate it. */
        sock->sockbase->vfptr->destroy (sock->sockbase);
        sock->state = GRID_SOCK_STATE_FINI;

        /*  Close the event FDs entirely. */
        if (!(sock->socktype->flags & GRID_SOCKTYPE_FLAG_NORECV)) {
            grid_efd_term (&sock->rcvfd);
        }
        if (!(sock->socktype->flags & GRID_SOCKTYPE_FLAG_NOSEND)) {
            grid_efd_term (&sock->sndfd);
        }

        /*  Now we can unblock the application thread blocked in
            the grid_close() call. */
        grid_sem_post (&sock->termsem);

        return;
    }

    grid_fsm_bad_state(sock->state, src, type);
}

static void grid_sock_handler (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    struct grid_sock *sock;
    struct grid_ep *ep;

    sock = grid_cont (self, struct grid_sock, fsm);

    switch (sock->state) {

/******************************************************************************/
/*  INIT state.                                                               */
/******************************************************************************/
    case GRID_SOCK_STATE_INIT:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                sock->state = GRID_SOCK_STATE_ACTIVE;
                return;
            case GRID_SOCK_ACTION_ZOMBIFY:
                grid_sock_action_zombify (sock);
                return;
            default:
                grid_fsm_bad_action (sock->state, src, type);
            }

        default:
            grid_fsm_bad_source (sock->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case GRID_SOCK_STATE_ACTIVE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_SOCK_ACTION_ZOMBIFY:
                grid_sock_action_zombify (sock);
                return;
            default:
                grid_fsm_bad_action (sock->state, src, type);
            }

        case GRID_SOCK_SRC_EP:
            switch (type) {
            case GRID_EP_STOPPED:

                /*  This happens when an endpoint is closed using
                    grid_shutdown() function. */
                ep = (struct grid_ep*) srcptr;
                grid_list_erase (&sock->sdeps, &ep->item);
                grid_ep_term (ep);
                grid_free (ep);
                return;

            default:
                grid_fsm_bad_action (sock->state, src, type);
            }

        default:

            /*  The assumption is that all the other events come from pipes. */
            switch (type) {
            case GRID_PIPE_IN:
                sock->sockbase->vfptr->in (sock->sockbase,
                    (struct grid_pipe*) srcptr);
                return;
            case GRID_PIPE_OUT:
                sock->sockbase->vfptr->out (sock->sockbase,
                    (struct grid_pipe*) srcptr);
                return;
            default:
                grid_fsm_bad_action (sock->state, src, type);
            }
        }

/******************************************************************************/
/*  ZOMBIE state.                                                             */
/******************************************************************************/
    case GRID_SOCK_STATE_ZOMBIE:
        grid_fsm_bad_state (sock->state, src, type);

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (sock->state, src, type);
    }
}

/******************************************************************************/
/*  State machine actions.                                                    */
/******************************************************************************/

static void grid_sock_action_zombify (struct grid_sock *self)
{
    /*  Switch to the zombie state. From now on all the socket
        functions will return ETERM. */
    self->state = GRID_SOCK_STATE_ZOMBIE;

    /*  Set IN and OUT events to unblock any polling function. */
    if (!(self->flags & GRID_SOCK_FLAG_IN)) {
        self->flags |= GRID_SOCK_FLAG_IN;
        if (!(self->socktype->flags & GRID_SOCKTYPE_FLAG_NORECV))
            grid_efd_signal (&self->rcvfd);
    }
    if (!(self->flags & GRID_SOCK_FLAG_OUT)) {
        self->flags |= GRID_SOCK_FLAG_OUT;
        if (!(self->socktype->flags & GRID_SOCKTYPE_FLAG_NOSEND))
            grid_efd_signal (&self->sndfd);
    }
}

void grid_sock_report_error (struct grid_sock *self, struct grid_ep *ep, int errnum)
{
    if (!grid_global_print_errors())
        return;

    if (errnum == 0)
        return;

    if (ep) {
        fprintf(stderr, "gridmq: socket.%s[%s]: Error: %s\n",
            self->socket_name, grid_ep_getaddr(ep), grid_strerror(errnum));
    } else {
        fprintf(stderr, "gridmq: socket.%s: Error: %s\n",
            self->socket_name, grid_strerror(errnum));
    }
}

void grid_sock_stat_increment (struct grid_sock *self, int name, int64_t increment)
{
    switch (name) {
        case GRID_STAT_ESTABLISHED_CONNECTIONS:
            grid_assert (increment > 0);
            self->statistics.established_connections += increment;
            break;
        case GRID_STAT_ACCEPTED_CONNECTIONS:
            grid_assert (increment > 0);
            self->statistics.accepted_connections += increment;
            break;
        case GRID_STAT_DROPPED_CONNECTIONS:
            grid_assert (increment > 0);
            self->statistics.dropped_connections += increment;
            break;
        case GRID_STAT_BROKEN_CONNECTIONS:
            grid_assert (increment > 0);
            self->statistics.broken_connections += increment;
            break;
        case GRID_STAT_CONNECT_ERRORS:
            grid_assert (increment > 0);
            self->statistics.connect_errors += increment;
            break;
        case GRID_STAT_BIND_ERRORS:
            grid_assert (increment > 0);
            self->statistics.bind_errors += increment;
            break;
        case GRID_STAT_ACCEPT_ERRORS:
            grid_assert (increment > 0);
            self->statistics.accept_errors += increment;
            break;
        case GRID_STAT_MESSAGES_SENT:
            grid_assert (increment > 0);
            self->statistics.messages_sent += increment;
            break;
        case GRID_STAT_MESSAGES_RECEIVED:
            grid_assert (increment > 0);
            self->statistics.messages_received += increment;
            break;
        case GRID_STAT_BYTES_SENT:
            grid_assert (increment >= 0);
            self->statistics.bytes_sent += increment;
            break;
        case GRID_STAT_BYTES_RECEIVED:
            grid_assert (increment >= 0);
            self->statistics.bytes_received += increment;
            break;

        case GRID_STAT_CURRENT_CONNECTIONS:
            grid_assert (increment > 0 ||
                self->statistics.current_connections >= -increment);
            grid_assert(increment < INT_MAX && increment > -INT_MAX);
            self->statistics.current_connections += (int) increment;
            break;
        case GRID_STAT_INPROGRESS_CONNECTIONS:
            grid_assert (increment > 0 ||
                self->statistics.inprogress_connections >= -increment);
            grid_assert(increment < INT_MAX && increment > -INT_MAX);
            self->statistics.inprogress_connections += (int) increment;
            break;
        case GRID_STAT_CURRENT_SND_PRIORITY:
            /*  This is an exception, we don't want to increment priority  */
            grid_assert((increment > 0 && increment <= 16) || increment == -1);
            self->statistics.current_snd_priority = (int) increment;
            break;
        case GRID_STAT_CURRENT_EP_ERRORS:
            grid_assert (increment > 0 ||
                self->statistics.current_ep_errors >= -increment);
            grid_assert(increment < INT_MAX && increment > -INT_MAX);
            self->statistics.current_ep_errors += (int) increment;
            break;
    }
}

int grid_sock_hold (struct grid_sock *self)
{
    switch (self->state) {
    case GRID_SOCK_STATE_ACTIVE:
    case GRID_SOCK_STATE_INIT:
        self->holds++;
        return 0;
    case GRID_SOCK_STATE_ZOMBIE:
        return -ETERM;
    case GRID_SOCK_STATE_STOPPING:
    case GRID_SOCK_STATE_STOPPING_EPS:
    case GRID_SOCK_STATE_FINI:
    default:
        return -EBADF;
    }
}

void grid_sock_rele (struct grid_sock *self)
{
    self->holds--;
    if (self->holds == 0) {
        grid_sem_post (&self->relesem);
    }
}

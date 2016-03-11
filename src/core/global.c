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

#include "../grid.h"
#include "../transport.h"
#include "../protocol.h"

#include "global.h"
#include "sock.h"
#include "ep.h"

#include "../aio/pool.h"
#include "../aio/timer.h"

#include "../utils/err.h"
#include "../utils/alloc.h"
#include "../utils/mutex.h"
#include "../utils/list.h"
#include "../utils/cont.h"
#include "../utils/random.h"
#include "../utils/glock.h"
#include "../utils/chunk.h"
#include "../utils/msg.h"
#include "../utils/attr.h"

#include "../transports/inproc/inproc.h"
#include "../transports/ipc/ipc.h"
#include "../transports/tcp/tcp.h"
#include "../transports/tcpmux/tcpmux.h"

#include "../protocols/pair/pair.h"
#include "../protocols/pair/xpair.h"
#include "../protocols/pubsub/pub.h"
#include "../protocols/pubsub/sub.h"
#include "../protocols/pubsub/xpub.h"
#include "../protocols/pubsub/xsub.h"
#include "../protocols/reqrep/rep.h"
#include "../protocols/reqrep/req.h"
#include "../protocols/reqrep/xrep.h"
#include "../protocols/reqrep/xreq.h"
#include "../protocols/pipeline/push.h"
#include "../protocols/pipeline/pull.h"
#include "../protocols/pipeline/xpush.h"
#include "../protocols/pipeline/xpull.h"
#include "../protocols/survey/respondent.h"
#include "../protocols/survey/surveyor.h"
#include "../protocols/survey/xrespondent.h"
#include "../protocols/survey/xsurveyor.h"
#include "../protocols/bus/bus.h"
#include "../protocols/bus/xbus.h"

#include "../pubsub.h"
#include "../pipeline.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GRID_HAVE_GMTIME_R


/*  Max number of concurrent SP sockets. */
#define GRID_MAX_SOCKETS 512

/*  To save some space, list of unused socket slots uses uint16_t integers to
    refer to individual sockets. If there's a need to more that 0x10000 sockets,
    the type should be changed to uint32_t or int. */
CT_ASSERT (GRID_MAX_SOCKETS <= 0x10000);

#define GRID_CTX_FLAG_ZOMBIE 1

#define GRID_GLOBAL_SRC_STAT_TIMER 1

#define GRID_GLOBAL_STATE_IDLE           1
#define GRID_GLOBAL_STATE_ACTIVE         2
#define GRID_GLOBAL_STATE_STOPPING_TIMER 3

struct grid_global {

    /*  The global table of existing sockets. The descriptor representing
        the socket is the index to this table. This pointer is also used to
        find out whether context is initialised. If it is NULL, context is
        uninitialised. */
    struct grid_sock **socks;

    /*  Stack of unused file descriptors. */
    uint16_t *unused;

    /*  Number of actual open sockets in the socket table. */
    size_t nsocks;

    /*  Combination of the flags listed above. */
    int flags;

    /*  List of all available transports.  Note that this list is not
        dynamic; i.e. it is created during global initialization and
        is never modified. */
    struct grid_list transports;

    /*  List of all available socket types.  Again this list is not dynamic.*/
    struct grid_list socktypes;

    /*  Pool of worker threads. */
    struct grid_pool pool;

    /*  Timer and other machinery for submitting statistics  */
    struct grid_ctx ctx;
    struct grid_fsm fsm;
    int state;
    struct grid_timer stat_timer;

    int print_errors;
    int print_statistics;

    /*  Special socket ids  */
    int statistics_socket;

    /*  Application name for statistics  */
    char hostname[64];
    char appname[64];
};

/*  Singleton object containing the global state of the library. */
static struct grid_global self;

/*  Context creation- and termination-related private functions. */
static void grid_global_init (void);
static void grid_global_term (void);

/*  Transport-related private functions. */
static void grid_global_add_transport (struct grid_transport *transport);
static void grid_global_add_socktype (struct grid_socktype *socktype);

/*  Private function that unifies grid_bind and grid_connect functionality.
    It returns the ID of the newly created endpoint. */
static int grid_global_create_ep (struct grid_sock *, const char *addr, int bind);

/*  Private socket creator which doesn't initialize global state and
    does no locking by itself */
static int grid_global_create_socket (int domain, int protocol);

/*  FSM callbacks  */
static void grid_global_handler (struct grid_fsm *self,
    int src, int type, void *srcptr);
static void grid_global_shutdown (struct grid_fsm *self,
    int src, int type, void *srcptr);

/*  Socket holds. */
static int grid_global_hold_socket(struct grid_sock **sockp, int s);
static int grid_global_hold_socket_locked(struct grid_sock **sockp, int s);
static void grid_global_rele_socket(struct grid_sock *);

int grid_errno (void)
{
    return grid_err_errno ();
}

const char *grid_strerror (int errnum)
{
    return grid_err_strerror (errnum);
}

static void grid_global_init (void)
{
    int i;
    char *envvar;
    int rc;
    char *addr;


    /*  Check whether the library was already initialised. If so, do nothing. */
    if (self.socks)
        return;

    /*  Initialise the memory allocation subsystem. */
    grid_alloc_init ();

    /*  Seed the pseudo-random number generator. */
    grid_random_seed ();

    /*  Allocate the global table of SP sockets. */
    self.socks = grid_alloc ((sizeof (struct grid_sock*) * GRID_MAX_SOCKETS) +
        (sizeof (uint16_t) * GRID_MAX_SOCKETS), "socket table");
    alloc_assert (self.socks);
    for (i = 0; i != GRID_MAX_SOCKETS; ++i)
        self.socks [i] = NULL;
    self.nsocks = 0;
    self.flags = 0;

    /*  Print connection and accepting errors to the stderr  */
    envvar = getenv("GRID_PRINT_ERRORS");
    /*  any non-empty string is true */
    self.print_errors = envvar && *envvar;

    /*  Print socket statistics to stderr  */
    envvar = getenv("GRID_PRINT_STATISTICS");
    self.print_statistics = envvar && *envvar;

    /*  Allocate the stack of unused file descriptors. */
    self.unused = (uint16_t*) (self.socks + GRID_MAX_SOCKETS);
    alloc_assert (self.unused);
    for (i = 0; i != GRID_MAX_SOCKETS; ++i)
        self.unused [i] = GRID_MAX_SOCKETS - i - 1;

    /*  Initialise other parts of the global state. */
    grid_list_init (&self.transports);
    grid_list_init (&self.socktypes);

    /*  Plug in individual transports. */
    grid_global_add_transport (grid_inproc);
    grid_global_add_transport (grid_ipc);
    grid_global_add_transport (grid_tcp);
    grid_global_add_transport (grid_tcpmux);

    /*  Plug in individual socktypes. */
    grid_global_add_socktype (grid_pair_socktype);
    grid_global_add_socktype (grid_xpair_socktype);
    grid_global_add_socktype (grid_pub_socktype);
    grid_global_add_socktype (grid_sub_socktype);
    grid_global_add_socktype (grid_xpub_socktype);
    grid_global_add_socktype (grid_xsub_socktype);
    grid_global_add_socktype (grid_rep_socktype);
    grid_global_add_socktype (grid_req_socktype);
    grid_global_add_socktype (grid_xrep_socktype);
    grid_global_add_socktype (grid_xreq_socktype);
    grid_global_add_socktype (grid_push_socktype);
    grid_global_add_socktype (grid_xpush_socktype);
    grid_global_add_socktype (grid_pull_socktype);
    grid_global_add_socktype (grid_xpull_socktype);
    grid_global_add_socktype (grid_respondent_socktype);
    grid_global_add_socktype (grid_surveyor_socktype);
    grid_global_add_socktype (grid_xrespondent_socktype);
    grid_global_add_socktype (grid_xsurveyor_socktype);
    grid_global_add_socktype (grid_bus_socktype);
    grid_global_add_socktype (grid_xbus_socktype);

    /*  Start the worker threads. */
    grid_pool_init (&self.pool);

    /*  Start FSM  */
    grid_fsm_init_root (&self.fsm, grid_global_handler, grid_global_shutdown,
        &self.ctx);
    self.state = GRID_GLOBAL_STATE_IDLE;

    grid_ctx_init (&self.ctx, grid_global_getpool (), NULL);
    grid_timer_init (&self.stat_timer, GRID_GLOBAL_SRC_STAT_TIMER, &self.fsm);

    /*   Initializing special sockets.  */
    addr = getenv ("GRID_STATISTICS_SOCKET");
    if (addr) {
        self.statistics_socket = grid_global_create_socket (AF_SP, GRID_PUB);
        errno_assert (self.statistics_socket >= 0);

        rc = grid_global_create_ep (self.socks[self.statistics_socket], addr, 0);
        errno_assert (rc >= 0);
    } else {
        self.statistics_socket = -1;
    }

    addr = getenv ("GRID_APPLICATION_NAME");
    if (addr) {
        strncpy (self.appname, addr, 63);
        self.appname[63] = '\0';
    } else {
        sprintf (self.appname, "gridmq.%d", getpid());
    }

    addr = getenv ("GRID_HOSTNAME");
    if (addr) {
        strncpy (self.hostname, addr, 63);
        self.hostname[63] = '\0';
    } else {
        rc = gethostname (self.hostname, 63);
        errno_assert (rc == 0);
        self.hostname[63] = '\0';
    }

    grid_fsm_start(&self.fsm);
}

static void grid_global_term (void)
{
    struct grid_list_item *it;
    struct grid_transport *tp;

    /*  If there are no sockets remaining, uninitialise the global context. */
    grid_assert (self.socks);
    if (self.nsocks > 0)
        return;

    /*  Stop the FSM  */
    grid_ctx_enter (&self.ctx);
    grid_fsm_stop (&self.fsm);
    grid_ctx_leave (&self.ctx);

    /*  Shut down the worker threads. */
    grid_pool_term (&self.pool);

    /* Terminate ctx mutex */
    grid_ctx_term (&self.ctx);

    /*  Ask all the transport to deallocate their global resources. */
    while (!grid_list_empty (&self.transports)) {
        it = grid_list_begin (&self.transports);
        tp = grid_cont (it, struct grid_transport, item);
        if (tp->term)
            tp->term ();
        grid_list_erase (&self.transports, it);
    }

    /*  For now there's nothing to deallocate about socket types, however,
        let's remove them from the list anyway. */
    while (!grid_list_empty (&self.socktypes))
        grid_list_erase (&self.socktypes, grid_list_begin (&self.socktypes));

    /*  Final deallocation of the grid_global object itself. */
    grid_list_term (&self.socktypes);
    grid_list_term (&self.transports);
    grid_free (self.socks);

    /*  This marks the global state as uninitialised. */
    self.socks = NULL;

    /*  Shut down the memory allocation subsystem. */
    grid_alloc_term ();
}

void grid_term (void)
{
    int i;

    grid_glock_lock ();

    /*  Switch the global state into the zombie state. */
    self.flags |= GRID_CTX_FLAG_ZOMBIE;

    /*  Mark all open sockets as terminating. */
    if (self.socks && self.nsocks) {
        for (i = 0; i != GRID_MAX_SOCKETS; ++i)
            if (self.socks [i])
                grid_sock_zombify (self.socks [i]);
    }

    grid_glock_unlock ();
}

void *grid_allocmsg (size_t size, int type)
{
    int rc;
    void *result;

    rc = grid_chunk_alloc (size, type, &result);
    if (rc == 0)
        return result;
    errno = -rc;
    return NULL;
}

void *grid_reallocmsg (void *msg, size_t size)
{
    int rc;

    rc = grid_chunk_realloc (size, &msg);
    if (rc == 0)
        return msg;
    errno = -rc;
    return NULL;
}

int grid_freemsg (void *msg)
{
    grid_chunk_free (msg);
    return 0;
}

struct grid_cmsghdr *grid_cmsg_nxthdr_ (const struct grid_msghdr *mhdr,
    const struct grid_cmsghdr *cmsg)
{
    char *data;
    size_t sz;
    struct grid_cmsghdr *next;
    size_t headsz;

    /*  Early return if no message is provided. */
    if (grid_slow (mhdr == NULL))
        return NULL;

    /*  Get the actual data. */
    if (mhdr->msg_controllen == GRID_MSG) {
        data = *((void**) mhdr->msg_control);
        sz = grid_chunk_size (data);
    }
    else {
        data = (char*) mhdr->msg_control;
        sz = mhdr->msg_controllen;
    }

    /*  Ancillary data allocation was not even large enough for one element. */
    if (grid_slow (sz < GRID_CMSG_SPACE (0)))
        return NULL;

    /*  If cmsg is set to NULL we are going to return first property.
        Otherwise move to the next property. */
    if (!cmsg)
        next = (struct grid_cmsghdr*) data;
    else
        next = (struct grid_cmsghdr*)
            (((char*) cmsg) + GRID_CMSG_ALIGN_ (cmsg->cmsg_len));

    /*  If there's no space for next property, treat it as the end
        of the property list. */
    headsz = ((char*) next) - data;
    if (headsz + GRID_CMSG_SPACE (0) > sz ||
          headsz + GRID_CMSG_ALIGN_ (next->cmsg_len) > sz)
        return NULL;
    
    /*  Success. */
    return next;
}

int grid_global_create_socket (int domain, int protocol)
{
    int rc;
    int s;
    struct grid_list_item *it;
    struct grid_socktype *socktype;
    struct grid_sock *sock;
    /* The function is called with grid_glock held */

    /*  Only AF_SP and AF_SP_RAW domains are supported. */
    if (grid_slow (domain != AF_SP && domain != AF_SP_RAW)) {
        return -EAFNOSUPPORT;
    }

    /*  If socket limit was reached, report error. */
    if (grid_slow (self.nsocks >= GRID_MAX_SOCKETS)) {
        return -EMFILE;
    }

    /*  Find an empty socket slot. */
    s = self.unused [GRID_MAX_SOCKETS - self.nsocks - 1];

    /*  Find the appropriate socket type. */
    for (it = grid_list_begin (&self.socktypes);
          it != grid_list_end (&self.socktypes);
          it = grid_list_next (&self.socktypes, it)) {
        socktype = grid_cont (it, struct grid_socktype, item);
        if (socktype->domain == domain && socktype->protocol == protocol) {

            /*  Instantiate the socket. */
            sock = grid_alloc (sizeof (struct grid_sock), "sock");
            alloc_assert (sock);
            rc = grid_sock_init (sock, socktype, s);
            if (rc < 0)
                return rc;

            /*  Adjust the global socket table. */
            self.socks [s] = sock;
            ++self.nsocks;
            return s;
        }
    }
    /*  Specified socket type wasn't found. */
    return -EINVAL;
}

int grid_socket (int domain, int protocol)
{
    int rc;

    grid_glock_lock ();

    /*  If grid_term() was already called, return ETERM. */
    if (grid_slow (self.flags & GRID_CTX_FLAG_ZOMBIE)) {
        grid_glock_unlock ();
        errno = ETERM;
        return -1;
    }

    /*  Make sure that global state is initialised. */
    grid_global_init ();

    rc = grid_global_create_socket (domain, protocol);

    if (rc < 0) {
        grid_global_term ();
        grid_glock_unlock ();
        errno = -rc;
        return -1;
    }

    grid_glock_unlock();

    return rc;
}

int grid_close (int s)
{
    int rc;
    struct grid_sock *sock;

    grid_glock_lock ();
    rc = grid_global_hold_socket_locked (&sock, s);
    if (grid_slow (rc < 0)) {
        grid_glock_unlock ();
        errno = -rc;
        return -1;
    }

    /*  Start the shutdown process on the socket.  This will cause
        all other socket users, as well as endpoints, to begin cleaning up.
        This is done with the glock held to ensure that two instances
        of grid_close can't access the same socket. */
    grid_sock_stop (sock);

    /*  We have to drop both the hold we just acquired, as well as
        the original hold, in order for grid_sock_term to complete. */
    grid_sock_rele (sock);
    grid_sock_rele (sock);
    grid_glock_unlock ();

    /*  Now clean up.  The termination routine below will block until
        all other consumers of the socket have dropped their holds, and
        all endpoints have cleanly exited. */
    rc = grid_sock_term (sock);
    if (grid_slow (rc == -EINTR)) {
        grid_global_rele_socket (sock);
        errno = EINTR;
        return -1;
    }

    /*  Remove the socket from the socket table, add it to unused socket
        table. */
    grid_glock_lock ();
    self.socks [s] = NULL;
    self.unused [GRID_MAX_SOCKETS - self.nsocks] = s;
    --self.nsocks;
    grid_free (sock);

    /*  Destroy the global context if there's no socket remaining. */
    grid_global_term ();

    grid_glock_unlock ();

    return 0;
}

int grid_setsockopt (int s, int level, int option, const void *optval,
    size_t optvallen)
{
    int rc;
    struct grid_sock *sock;

    rc = grid_global_hold_socket (&sock, s);
    if (grid_slow (rc < 0)) {
        errno = -rc;
        return -1;
    }

    if (grid_slow (!optval && optvallen)) {
        rc = -EFAULT;
        goto fail;
    }

    rc = grid_sock_setopt (sock, level, option, optval, optvallen);
    if (grid_slow (rc < 0))
        goto fail;
    errnum_assert (rc == 0, -rc);
    grid_global_rele_socket (sock);
    return 0;

fail:
    grid_global_rele_socket (sock);
    errno = -rc;
    return -1;
}

int grid_getsockopt (int s, int level, int option, void *optval,
    size_t *optvallen)
{
    int rc;
    struct grid_sock *sock;

    rc = grid_global_hold_socket (&sock, s);
    if (grid_slow (rc < 0)) {
        errno = -rc;
        return -1;
    }

    if (grid_slow (!optval && optvallen)) {
        rc = -EFAULT;
        goto fail;
    }

    rc = grid_sock_getopt (sock, level, option, optval, optvallen);
    if (grid_slow (rc < 0))
        goto fail;
    errnum_assert (rc == 0, -rc);
    grid_global_rele_socket (sock);
    return 0;

fail:
    grid_global_rele_socket (sock);
    errno = -rc;
    return -1;
}

int grid_bind (int s, const char *addr)
{
    int rc;
    struct grid_sock *sock;

    rc = grid_global_hold_socket (&sock, s);
    if (rc < 0) {
        errno = -rc;
        return -1;
    }

    rc = grid_global_create_ep (sock, addr, 1);
    if (grid_slow (rc < 0)) {
        grid_global_rele_socket (sock);
        errno = -rc;
        return -1;
    }

    grid_global_rele_socket (sock);
    return rc;
}

int grid_connect (int s, const char *addr)
{
    int rc;
    struct grid_sock *sock;

    rc = grid_global_hold_socket (&sock, s);
    if (grid_slow (rc < 0)) {
        errno = -rc;
        return -1;
    }

    rc = grid_global_create_ep (sock, addr, 0);
    if (rc < 0) {
        grid_global_rele_socket (sock);
        errno = -rc;
        return -1;
    }

    grid_global_rele_socket (sock);
    return rc;
}

int grid_shutdown (int s, int how)
{
    int rc;
    struct grid_sock *sock;

    rc = grid_global_hold_socket (&sock, s);
    if (grid_slow (rc < 0)) {
        errno = -rc;
        return -1;
    }

    rc = grid_sock_rm_ep (sock, how);
    if (grid_slow (rc < 0)) {
        grid_global_rele_socket (sock);
        errno = -rc;
        return -1;
    }
    grid_assert (rc == 0);

    grid_global_rele_socket (sock);
    return 0;
}

int grid_send (int s, const void *buf, size_t len, int flags)
{
    struct grid_iovec iov;
    struct grid_msghdr hdr;

    iov.iov_base = (void*) buf;
    iov.iov_len = len;

    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = NULL;
    hdr.msg_controllen = 0;

    return grid_sendmsg (s, &hdr, flags);
}

int grid_recv (int s, void *buf, size_t len, int flags)
{
    struct grid_iovec iov;
    struct grid_msghdr hdr;

    iov.iov_base = buf;
    iov.iov_len = len;

    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = NULL;
    hdr.msg_controllen = 0;

    return grid_recvmsg (s, &hdr, flags);
}

int grid_sendmsg (int s, const struct grid_msghdr *msghdr, int flags)
{
    int rc;
    size_t sz;
    size_t spsz;
    int i;
    struct grid_iovec *iov;
    struct grid_msg msg;
    void *chunk;
    int nnmsg;
    struct grid_cmsghdr *cmsg;
    struct grid_sock *sock;

    rc = grid_global_hold_socket (&sock, s);
    if (grid_slow (rc < 0)) {
        errno = -rc;
        return -1;
    }

    if (grid_slow (!msghdr)) {
        rc = -EINVAL;
        goto fail;
    }

    if (grid_slow (msghdr->msg_iovlen < 0)) {
        rc = -EMSGSIZE;
        goto fail;
    }

    if (msghdr->msg_iovlen == 1 && msghdr->msg_iov [0].iov_len == GRID_MSG) {
        chunk = *(void**) msghdr->msg_iov [0].iov_base;
        if (grid_slow (chunk == NULL)) {
            rc = -EFAULT;
            goto fail;
        }
        sz = grid_chunk_size (chunk);
        grid_msg_init_chunk (&msg, chunk);
        nnmsg = 1;
    }
    else {

        /*  Compute the total size of the message. */
        sz = 0;
        for (i = 0; i != msghdr->msg_iovlen; ++i) {
            iov = &msghdr->msg_iov [i];
            if (grid_slow (iov->iov_len == GRID_MSG)) {
               rc = -EINVAL;
               goto fail;
            }
            if (grid_slow (!iov->iov_base && iov->iov_len)) {
                rc = -EFAULT;
                goto fail;
            }
            if (grid_slow (sz + iov->iov_len < sz)) {
                rc = -EINVAL;
                goto fail;
            }
            sz += iov->iov_len;
        }

        /*  Create a message object from the supplied scatter array. */
        grid_msg_init (&msg, sz);
        sz = 0;
        for (i = 0; i != msghdr->msg_iovlen; ++i) {
            iov = &msghdr->msg_iov [i];
            memcpy (((uint8_t*) grid_chunkref_data (&msg.body)) + sz,
                iov->iov_base, iov->iov_len);
            sz += iov->iov_len;
        }

        nnmsg = 0;
    }

    /*  Add ancillary data to the message. */
    if (msghdr->msg_control) {

        /*  Copy all headers. */
        /*  TODO: SP_HDR should not be copied here! */
        if (msghdr->msg_controllen == GRID_MSG) {
            chunk = *((void**) msghdr->msg_control);
            grid_chunkref_term (&msg.hdrs);
            grid_chunkref_init_chunk (&msg.hdrs, chunk);
        }
        else {
            grid_chunkref_term (&msg.hdrs);
            grid_chunkref_init (&msg.hdrs, msghdr->msg_controllen);
            memcpy (grid_chunkref_data (&msg.hdrs),
                msghdr->msg_control, msghdr->msg_controllen);
        }

        /* Search for SP_HDR property. */
        cmsg = GRID_CMSG_FIRSTHDR (msghdr);
        while (cmsg) {
            if (cmsg->cmsg_level == PROTO_SP && cmsg->cmsg_type == SP_HDR) {
                unsigned char *ptr = GRID_CMSG_DATA (cmsg);
                size_t clen = cmsg->cmsg_len - GRID_CMSG_SPACE (0);
                if (clen > sizeof (size_t)) {
                    spsz = *(size_t *)(void *)ptr;
                    if (spsz <= (clen - sizeof (size_t))) {
                        /*  Copy body of SP_HDR property into 'sphdr'. */
                        grid_chunkref_term (&msg.sphdr);
                        grid_chunkref_init (&msg.sphdr, spsz);
                         memcpy (grid_chunkref_data (&msg.sphdr),
                             ptr + sizeof (size_t), spsz);
                    }
                }
                break;
            }
            cmsg = GRID_CMSG_NXTHDR (msghdr, cmsg);
        }
    }

    /*  Send it further down the stack. */
    rc = grid_sock_send (sock, &msg, flags);
    if (grid_slow (rc < 0)) {

        /*  If we are dealing with user-supplied buffer, detach it from
            the message object. */
        if (nnmsg)
            grid_chunkref_init (&msg.body, 0);

        grid_msg_term (&msg);
        goto fail;
    }

    /*  Adjust the statistics. */
    grid_sock_stat_increment (sock, GRID_STAT_MESSAGES_SENT, 1);
    grid_sock_stat_increment (sock, GRID_STAT_BYTES_SENT, sz);

    grid_global_rele_socket (sock);

    return (int) sz;

fail:
    grid_global_rele_socket (sock);

    errno = -rc;
    return -1;
}

int grid_recvmsg (int s, struct grid_msghdr *msghdr, int flags)
{
    int rc;
    struct grid_msg msg;
    uint8_t *data;
    size_t sz;
    int i;
    struct grid_iovec *iov;
    void *chunk;
    size_t hdrssz;
    void *ctrl;
    size_t ctrlsz;
    size_t spsz;
    size_t sptotalsz;
    struct grid_cmsghdr *chdr;
    struct grid_sock *sock;

    rc = grid_global_hold_socket (&sock, s);
    if (grid_slow (rc < 0)) {
        errno = -rc;
        return -1;
    }

    if (grid_slow (!msghdr)) {
        rc = -EINVAL;
        goto fail;
    }

    if (grid_slow (msghdr->msg_iovlen < 0)) {
        rc = -EMSGSIZE;
        goto fail;
    }

    /*  Get a message. */
    rc = grid_sock_recv (sock, &msg, flags);
    if (grid_slow (rc < 0)) {
        goto fail;
    }

    if (msghdr->msg_iovlen == 1 && msghdr->msg_iov [0].iov_len == GRID_MSG) {
        chunk = grid_chunkref_getchunk (&msg.body);
        *(void**) (msghdr->msg_iov [0].iov_base) = chunk;
        sz = grid_chunk_size (chunk);
    }
    else {

        /*  Copy the message content into the supplied gather array. */
        data = grid_chunkref_data (&msg.body);
        sz = grid_chunkref_size (&msg.body);
        for (i = 0; i != msghdr->msg_iovlen; ++i) {
            iov = &msghdr->msg_iov [i];
            if (grid_slow (iov->iov_len == GRID_MSG)) {
                grid_msg_term (&msg);
                rc = -EINVAL;
                goto fail;
            }
            if (iov->iov_len > sz) {
                memcpy (iov->iov_base, data, sz);
                break;
            }
            memcpy (iov->iov_base, data, iov->iov_len);
            data += iov->iov_len;
            sz -= iov->iov_len;
        }
        sz = grid_chunkref_size (&msg.body);
    }

    /*  Retrieve the ancillary data from the message. */
    if (msghdr->msg_control) {

        spsz = grid_chunkref_size (&msg.sphdr);
        sptotalsz = GRID_CMSG_SPACE (spsz+sizeof (size_t));
        ctrlsz = sptotalsz + grid_chunkref_size (&msg.hdrs);

        if (msghdr->msg_controllen == GRID_MSG) {

            /* Allocate the buffer. */
            rc = grid_chunk_alloc (ctrlsz, 0, &ctrl);
            errnum_assert (rc == 0, -rc);

            /* Set output parameters. */
            *((void**) msghdr->msg_control) = ctrl;
        }
        else {

            /* Just use the buffer supplied by the user. */
            ctrl = msghdr->msg_control;
            ctrlsz = msghdr->msg_controllen;
        }

        /* If SP header alone won't fit into the buffer, return no ancillary
           properties. */
        if (ctrlsz >= sptotalsz) {
            char *ptr;

            /*  Fill in SP_HDR ancillary property. */
            chdr = (struct grid_cmsghdr*) ctrl;
            chdr->cmsg_len = sptotalsz;
            chdr->cmsg_level = PROTO_SP;
            chdr->cmsg_type = SP_HDR;
            ptr = (void *)chdr;
            ptr += sizeof (*chdr);
            *(size_t *)(void *)ptr = spsz;
            ptr += sizeof (size_t);
            memcpy (ptr, grid_chunkref_data (&msg.sphdr), spsz);

            /*  Fill in as many remaining properties as possible.
                Truncate the trailing properties if necessary. */
            hdrssz = grid_chunkref_size (&msg.hdrs);
            if (hdrssz > ctrlsz - sptotalsz)
                hdrssz = ctrlsz - sptotalsz;
            memcpy (((char*) ctrl) + sptotalsz,
                grid_chunkref_data (&msg.hdrs), hdrssz);
        }
    }

    grid_msg_term (&msg);

    /*  Adjust the statistics. */
    grid_sock_stat_increment (sock, GRID_STAT_MESSAGES_RECEIVED, 1);
    grid_sock_stat_increment (sock, GRID_STAT_BYTES_RECEIVED, sz);

    grid_global_rele_socket (sock);

    return (int) sz;

fail:
    grid_global_rele_socket (sock);

    errno = -rc;
    return -1;
}

static void grid_global_add_transport (struct grid_transport *transport)
{
    if (transport->init)
        transport->init ();
    grid_list_insert (&self.transports, &transport->item,
        grid_list_end (&self.transports));
}

static void grid_global_add_socktype (struct grid_socktype *socktype)
{
    grid_list_insert (&self.socktypes, &socktype->item,
        grid_list_end (&self.socktypes));
}

static void grid_global_submit_counter (int i, struct grid_sock *s,
    char *name, uint64_t value)
{
    /* Length of buffer is:
       len(hostname) + len(appname) + len(socket_name) + len(timebuf)
       + len(str(value)) + len(static characters)
       63 + 63 + 63 + 20 + 20 + 60 = 289 */
    char buf[512];
    char timebuf[20];
    time_t numtime;
    struct tm strtime;
    int len;

    if(self.print_statistics) {
        fprintf(stderr, "gridmq: socket.%s: %s: %llu\n",
            s->socket_name, name, (long long unsigned int)value);
    }

    if (self.statistics_socket >= 0) {
        /*  TODO(tailhook) add HAVE_GMTIME_R ifdef  */
        time(&numtime);
#ifdef GRID_HAVE_GMTIME_R
        gmtime_r (&numtime, &strtime);
#else
#error
#endif
        strftime (timebuf, 20, "%Y-%m-%dT%H:%M:%S", &strtime);
        if(*s->socket_name) {
            len = sprintf (buf, "ESTP:%s:%s:socket.%s:%s: %sZ 10 %llu:c",
                self.hostname, self.appname, s->socket_name, name,
                timebuf, (long long unsigned int)value);
        } else {
            len = sprintf (buf, "ESTP:%s:%s:socket.%d:%s: %sZ 10 %llu:c",
                self.hostname, self.appname, i, name,
                timebuf, (long long unsigned int)value);
        }
        grid_assert (len < (int)sizeof(buf));
        (void) grid_send (self.statistics_socket, buf, len, GRID_DONTWAIT);
    }
}

static void grid_global_submit_level (int i, struct grid_sock *s,
    char *name, int value)
{
    /* Length of buffer is:
       len(hostname) + len(appname) + len(socket_name) + len(timebuf)
       + len(str(value)) + len(static characters)
       63 + 63 + 63 + 20 + 20 + 60 = 289 */
    char buf[512];
    char timebuf[20];
    time_t numtime;
    struct tm strtime;
    int len;

    if(self.print_statistics) {
        fprintf(stderr, "gridmq: socket.%s: %s: %d\n",
            s->socket_name, name, value);
    }

    if (self.statistics_socket >= 0) {
        /*  TODO(tailhook) add HAVE_GMTIME_R ifdef  */
        time(&numtime);
#ifdef GRID_HAVE_GMTIME_R
        gmtime_r (&numtime, &strtime);
#else
#error
#endif
        strftime (timebuf, 20, "%Y-%m-%dT%H:%M:%S", &strtime);
        if(*s->socket_name) {
            len = sprintf (buf, "ESTP:%s:%s:socket.%s:%s: %sZ 10 %d",
                self.hostname, self.appname, s->socket_name, name,
                timebuf, value);
        } else {
            len = sprintf (buf, "ESTP:%s:%s:socket.%d:%s: %sZ 10 %d",
                self.hostname, self.appname, i, name,
                timebuf, value);
        }
        grid_assert (len < (int)sizeof(buf));
        (void) grid_send (self.statistics_socket, buf, len, GRID_DONTWAIT);
    }
}

static void grid_global_submit_errors (int i, struct grid_sock *s,
    char *name, int value)
{
    /*  TODO(tailhook) dynamically allocate buffer  */
    char buf[4096];
    char *curbuf;
    int buf_left;
    char timebuf[20];
    time_t numtime;
    struct tm strtime;
    int len;
    struct grid_list_item *it;
    struct grid_ep *ep;

    if (self.statistics_socket >= 0) {
        /*  TODO(tailhook) add HAVE_GMTIME_R ifdef  */
        time(&numtime);
#ifdef GRID_HAVE_GMTIME_R
        gmtime_r (&numtime, &strtime);
#else
#error
#endif
        strftime (timebuf, 20, "%Y-%m-%dT%H:%M:%S", &strtime);
        if(*s->socket_name) {
            len = sprintf (buf, "ESTP:%s:%s:socket.%s:%s: %sZ 10 %d\n",
                self.hostname, self.appname, s->socket_name, name,
                timebuf, value);
        } else {
            len = sprintf (buf, "ESTP:%s:%s:socket.%d:%s: %sZ 10 %d\n",
                self.hostname, self.appname, i, name,
                timebuf, value);
        }
        buf_left = sizeof(buf) - len;
        curbuf = buf + len;


        for (it = grid_list_begin (&s->eps);
              it != grid_list_end (&s->eps);
              it = grid_list_next (&s->eps, it)) {
            ep = grid_cont (it, struct grid_ep, item);

            if (ep->last_errno) {
                 len = snprintf (curbuf, buf_left,
                     " gridmq: Endpoint %d [%s] error: %s\n",
                     ep->eid, grid_ep_getaddr (ep), grid_strerror (ep->last_errno));
                if (buf_left < len)
                    break;
                curbuf += len;
                buf_left -= len;
            }

        }

        (void) grid_send (self.statistics_socket,
            buf, sizeof(buf) - buf_left, GRID_DONTWAIT);
    }
}

static void grid_global_submit_statistics ()
{
    int i;
    struct grid_sock *s;

    /*  TODO(tailhook)  optimized it to use nsocks and unused  */
    for(i = 0; i < GRID_MAX_SOCKETS; ++i) {

        grid_glock_lock ();
        s = self.socks [i];
        if (!s) {
            grid_glock_unlock ();
            continue;
        }
        if (i == self.statistics_socket) {
            grid_glock_unlock ();
            continue;
        }
        grid_ctx_enter (&s->ctx);
        grid_glock_unlock ();

        grid_global_submit_counter (i, s,
            "established_connections", s->statistics.established_connections);
        grid_global_submit_counter (i, s,
            "accepted_connections", s->statistics.accepted_connections);
        grid_global_submit_counter (i, s,
            "dropped_connections", s->statistics.dropped_connections);
        grid_global_submit_counter (i, s,
            "broken_connections", s->statistics.broken_connections);
        grid_global_submit_counter (i, s,
            "connect_errors", s->statistics.connect_errors);
        grid_global_submit_counter (i, s,
            "bind_errors", s->statistics.bind_errors);
        grid_global_submit_counter (i, s,
            "accept_errors", s->statistics.accept_errors);
        grid_global_submit_counter (i, s,
            "messages_sent", s->statistics.messages_sent);
        grid_global_submit_counter (i, s,
            "messages_received", s->statistics.messages_received);
        grid_global_submit_counter (i, s,
            "bytes_sent", s->statistics.bytes_sent);
        grid_global_submit_counter (i, s,
            "bytes_received", s->statistics.bytes_received);
        grid_global_submit_level (i, s,
            "current_connections", s->statistics.current_connections);
        grid_global_submit_level (i, s,
            "inprogress_connections", s->statistics.inprogress_connections);
        grid_global_submit_level (i, s,
            "current_snd_priority", s->statistics.current_snd_priority);
        grid_global_submit_errors (i, s,
            "current_ep_errors", s->statistics.current_ep_errors);
        grid_ctx_leave (&s->ctx);
    }
}

static int grid_global_create_ep (struct grid_sock *sock, const char *addr,
    int bind)
{
    int rc;
    const char *proto;
    const char *delim;
    size_t protosz;
    struct grid_transport *tp;
    struct grid_list_item *it;

    /*  Check whether address is valid. */
    if (!addr)
        return -EINVAL;
    if (strlen (addr) >= GRID_SOCKADDR_MAX)
        return -ENAMETOOLONG;

    /*  Separate the protocol and the actual address. */
    proto = addr;
    delim = strchr (addr, ':');
    if (!delim)
        return -EINVAL;
    if (delim [1] != '/' || delim [2] != '/')
        return -EINVAL;
    protosz = delim - addr;
    addr += protosz + 3;

    /*  Find the specified protocol. */
    tp = NULL;
    for (it = grid_list_begin (&self.transports);
          it != grid_list_end (&self.transports);
          it = grid_list_next (&self.transports, it)) {
        tp = grid_cont (it, struct grid_transport, item);
        if (strlen (tp->name) == protosz &&
              memcmp (tp->name, proto, protosz) == 0)
            break;
        tp = NULL;
    }

    /*  The protocol specified doesn't match any known protocol. */
    if (!tp) {
        return -EPROTONOSUPPORT;
    }

    /*  Ask the socket to create the endpoint. */
    rc = grid_sock_add_ep (sock, tp, bind, addr);
    return rc;
}

struct grid_transport *grid_global_transport (int id)
{
    struct grid_transport *tp;
    struct grid_list_item *it;

    /*  Find the specified protocol. */
    tp = NULL;
    for (it = grid_list_begin (&self.transports);
          it != grid_list_end (&self.transports);
          it = grid_list_next (&self.transports, it)) {
        tp = grid_cont (it, struct grid_transport, item);
        if (tp->id == id)
            break;
        tp = NULL;
    }

    return tp;
}

struct grid_pool *grid_global_getpool ()
{
    return &self.pool;
}

static void grid_global_handler (struct grid_fsm *self,
    int src, int type, GRID_UNUSED void *srcptr)
{

    struct grid_global *global;

    global = grid_cont (self, struct grid_global, fsm);

    switch (global->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  The state machine wasn't yet started.                                     */
/******************************************************************************/
    case GRID_GLOBAL_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                global->state = GRID_GLOBAL_STATE_ACTIVE;
                if (global->print_statistics || global->statistics_socket >= 0)
                {
                    /*  Start statistics collection timer. */
                    grid_timer_start (&global->stat_timer, 10000);
                }
                return;
            default:
                grid_fsm_bad_action (global->state, src, type);
            }

        default:
            grid_fsm_bad_source (global->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  Normal lifetime for global object.                                        */
/******************************************************************************/
    case GRID_GLOBAL_STATE_ACTIVE:
        switch (src) {

        case GRID_GLOBAL_SRC_STAT_TIMER:
            switch (type) {
            case GRID_TIMER_TIMEOUT:
                grid_global_submit_statistics ();
                /*  No need to change state  */
                grid_timer_stop (&global->stat_timer);
                return;
            case GRID_TIMER_STOPPED:
                grid_timer_start (&global->stat_timer, 10000);
                return;
            default:
                grid_fsm_bad_action (global->state, src, type);
            }

        default:
            grid_fsm_bad_source (global->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (global->state, src, type);
    }
}

static void grid_global_shutdown (struct grid_fsm *self,
    GRID_UNUSED int src, GRID_UNUSED int type, GRID_UNUSED void *srcptr)
{

    struct grid_global *global;

    global = grid_cont (self, struct grid_global, fsm);

    grid_assert (global->state == GRID_GLOBAL_STATE_ACTIVE
        || global->state == GRID_GLOBAL_STATE_IDLE);
    if (global->state == GRID_GLOBAL_STATE_ACTIVE) {
        if (!grid_timer_isidle (&global->stat_timer)) {
            grid_timer_stop (&global->stat_timer);
            return;
        }
    }
}

int grid_global_print_errors () {
    return self.print_errors;
}

/*  Get the socket structure for a socket id.  This must be called under
    the global lock (grid_glock_lock.)  The socket itself will not be freed
    while the hold is active. */
int grid_global_hold_socket_locked(struct grid_sock **sockp, int s)
{
    struct grid_sock *sock;

    if (grid_slow (self.socks == NULL)) {
        *sockp = NULL;
        return -ETERM;
    }
    if (grid_slow ((self.flags & GRID_CTX_FLAG_ZOMBIE) != 0)) {
        *sockp = NULL;
        return -ETERM;
    }

    if (grid_slow (s < 0 || s >= GRID_MAX_SOCKETS))
        return -EBADF;

    sock = self.socks[s];
    if (grid_slow (sock == NULL))
        return -EBADF;

    if (grid_slow (grid_sock_hold (sock) != 0)) {
        return -EBADF;
    }
    *sockp = sock;
    return 0;
}

int grid_global_hold_socket(struct grid_sock **sockp, int s)
{
    int rc;
    grid_glock_lock();
    rc = grid_global_hold_socket_locked(sockp, s);
    grid_glock_unlock();
    return rc;
}

void grid_global_rele_socket(struct grid_sock *sock)
{
    grid_glock_lock();
    grid_sock_rele(sock);
    grid_glock_unlock();
}

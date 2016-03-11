/*
    Copyright (c) 2012-2014 Martin Sustrik  All rights reserved.

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

#include "ctcpmux.h"
#include "stcpmux.h"

#include "../../tcpmux.h"

#include "../utils/dns.h"
#include "../utils/port.h"
#include "../utils/iface.h"
#include "../utils/backoff.h"
#include "../utils/literal.h"

#include "../../aio/fsm.h"
#include "../../aio/usock.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/fast.h"
#include "../../utils/int.h"
#include "../../utils/attr.h"

#include <string.h>

#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define GRID_CTCPMUX_STATE_IDLE 1
#define GRID_CTCPMUX_STATE_RESOLVING 2
#define GRID_CTCPMUX_STATE_STOPPING_DNS 3
#define GRID_CTCPMUX_STATE_CONNECTING 4
#define GRID_CTCPMUX_STATE_SENDING_TCPMUXHDR 5
#define GRID_CTCPMUX_STATE_RECEIVING_TCPMUXHDR 6
#define GRID_CTCPMUX_STATE_ACTIVE 7
#define GRID_CTCPMUX_STATE_STOPPING_STCPMUX 8
#define GRID_CTCPMUX_STATE_STOPPING_USOCK 9
#define GRID_CTCPMUX_STATE_WAITING 10
#define GRID_CTCPMUX_STATE_STOPPING_BACKOFF 11
#define GRID_CTCPMUX_STATE_STOPPING_STCPMUX_FINAL 12
#define GRID_CTCPMUX_STATE_STOPPING 13

#define GRID_CTCPMUX_SRC_USOCK 1
#define GRID_CTCPMUX_SRC_RECONNECT_TIMER 2
#define GRID_CTCPMUX_SRC_DNS 3
#define GRID_CTCPMUX_SRC_STCPMUX 4

struct grid_ctcpmux {

    /*  The state machine. */
    struct grid_fsm fsm;
    int state;

    /*  This object is a specific type of endpoint.
        Thus it is derived from epbase. */
    struct grid_epbase epbase;

    /*  The underlying TCPMUX socket. */
    struct grid_usock usock;

    /*  Used to wait before retrying to connect. */
    struct grid_backoff retry;

    /*  State machine that handles the active part of the connection
        lifetime. */
    struct grid_stcpmux stcpmux;

    /*  DNS resolver used to convert textual address into actual IP address
        along with the variable to hold the result. */
    struct grid_dns dns;
    struct grid_dns_result dns_result;

    /*  Buffer used in TCPMUX header exchange. */
    char buffer [256];
};

/*  grid_epbase virtual interface implementation. */
static void grid_ctcpmux_stop (struct grid_epbase *self);
static void grid_ctcpmux_destroy (struct grid_epbase *self);
const struct grid_epbase_vfptr grid_ctcpmux_epbase_vfptr = {
    grid_ctcpmux_stop,
    grid_ctcpmux_destroy
};

/*  Private functions. */
static void grid_ctcpmux_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_ctcpmux_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_ctcpmux_start_resolving (struct grid_ctcpmux *self);
static void grid_ctcpmux_start_connecting (struct grid_ctcpmux *self,
    struct sockaddr_storage *ss, size_t sslen);

int grid_ctcpmux_create (void *hint, struct grid_epbase **epbase)
{
    int rc;
    const char *addr;
    size_t addrlen;
    const char *semicolon;
    const char *hostname;
    const char *colon;
    const char *slash;
    const char *end;
    struct sockaddr_storage ss;
    size_t sslen;
    int ipv4only;
    size_t ipv4onlylen;
    struct grid_ctcpmux *self;
    int reconnect_ivl;
    int reconnect_ivl_max;
    size_t sz;

    /*  Allocate the new endpoint object. */
    self = grid_alloc (sizeof (struct grid_ctcpmux), "ctcpmux");
    alloc_assert (self);

    /*  Initalise the endpoint. */
    grid_epbase_init (&self->epbase, &grid_ctcpmux_epbase_vfptr, hint);

    /*  Check whether IPv6 is to be used. */
    ipv4onlylen = sizeof (ipv4only);
    grid_epbase_getopt (&self->epbase, GRID_SOL_SOCKET, GRID_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    grid_assert (ipv4onlylen == sizeof (ipv4only));

    /*  Start parsing the address. */
    addr = grid_epbase_getaddr (&self->epbase);
    addrlen = strlen (addr);
    semicolon = strchr (addr, ';');
    hostname = semicolon != NULL ? semicolon + 1 : addr;
    colon = strrchr (addr, ':');
    if (colon != NULL) {
        slash = strchr (colon + 1, '/');
    } else {
        slash = NULL;
    }
    end = addr + addrlen;

    if (grid_slow (!colon || !slash ||
          (size_t)(end - (slash + 1)) > sizeof (self->buffer) - 3)) {
        grid_epbase_term (&self->epbase);
        return -EINVAL;
    }

    /*  Parse the port. */
    rc = grid_port_resolve (colon + 1, slash  - colon - 1);
    if (grid_slow (rc < 0)) {
        grid_epbase_term (&self->epbase);
        return -EINVAL;
    }

    /*  Check whether the host portion of the address is either a literal
        or a valid hostname. */
    if (grid_dns_check_hostname (hostname, colon - hostname) < 0 &&
          grid_literal_resolve (hostname, colon - hostname, ipv4only,
          &ss, &sslen) < 0) {
        grid_epbase_term (&self->epbase);
        return -EINVAL;
    }

    /*  If local address is specified, check whether it is valid. */
    if (semicolon) {
        rc = grid_iface_resolve (addr, semicolon - addr, ipv4only, &ss, &sslen);
        if (rc < 0) {
            grid_epbase_term (&self->epbase);
            return -ENODEV;
        }
    }

    /*  Initialise the structure. */
    grid_fsm_init_root (&self->fsm, grid_ctcpmux_handler, grid_ctcpmux_shutdown,
        grid_epbase_getctx (&self->epbase));
    self->state = GRID_CTCPMUX_STATE_IDLE;
    grid_usock_init (&self->usock, GRID_CTCPMUX_SRC_USOCK, &self->fsm);
    sz = sizeof (reconnect_ivl);
    grid_epbase_getopt (&self->epbase, GRID_SOL_SOCKET, GRID_RECONNECT_IVL,
        &reconnect_ivl, &sz);
    grid_assert (sz == sizeof (reconnect_ivl));
    sz = sizeof (reconnect_ivl_max);
    grid_epbase_getopt (&self->epbase, GRID_SOL_SOCKET, GRID_RECONNECT_IVL_MAX,
        &reconnect_ivl_max, &sz);
    grid_assert (sz == sizeof (reconnect_ivl_max));
    if (reconnect_ivl_max == 0)
        reconnect_ivl_max = reconnect_ivl;
    grid_backoff_init (&self->retry, GRID_CTCPMUX_SRC_RECONNECT_TIMER,
        reconnect_ivl, reconnect_ivl_max, &self->fsm);
    grid_stcpmux_init (&self->stcpmux, GRID_CTCPMUX_SRC_STCPMUX, &self->epbase, &self->fsm);
    grid_dns_init (&self->dns, GRID_CTCPMUX_SRC_DNS, &self->fsm);

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);

    /*  Return the base class as an out parameter. */
    *epbase = &self->epbase;

    return 0;
}

static void grid_ctcpmux_stop (struct grid_epbase *self)
{
    struct grid_ctcpmux *ctcpmux;

    ctcpmux = grid_cont (self, struct grid_ctcpmux, epbase);

    grid_fsm_stop (&ctcpmux->fsm);
}

static void grid_ctcpmux_destroy (struct grid_epbase *self)
{
    struct grid_ctcpmux *ctcpmux;

    ctcpmux = grid_cont (self, struct grid_ctcpmux, epbase);

    grid_dns_term (&ctcpmux->dns);
    grid_stcpmux_term (&ctcpmux->stcpmux);
    grid_backoff_term (&ctcpmux->retry);
    grid_usock_term (&ctcpmux->usock);
    grid_fsm_term (&ctcpmux->fsm);
    grid_epbase_term (&ctcpmux->epbase);

    grid_free (ctcpmux);
}

static void grid_ctcpmux_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_ctcpmux *ctcpmux;

    ctcpmux = grid_cont (self, struct grid_ctcpmux, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        if (!grid_stcpmux_isidle (&ctcpmux->stcpmux)) {
            grid_epbase_stat_increment (&ctcpmux->epbase,
                GRID_STAT_DROPPED_CONNECTIONS, 1);
            grid_stcpmux_stop (&ctcpmux->stcpmux);
        }
        ctcpmux->state = GRID_CTCPMUX_STATE_STOPPING_STCPMUX_FINAL;
    }
    if (grid_slow (ctcpmux->state == GRID_CTCPMUX_STATE_STOPPING_STCPMUX_FINAL)) {
        if (!grid_stcpmux_isidle (&ctcpmux->stcpmux))
            return;
        grid_backoff_stop (&ctcpmux->retry);
        grid_usock_stop (&ctcpmux->usock);
        grid_dns_stop (&ctcpmux->dns);
        ctcpmux->state = GRID_CTCPMUX_STATE_STOPPING;
    }
    if (grid_slow (ctcpmux->state == GRID_CTCPMUX_STATE_STOPPING)) {
        if (!grid_backoff_isidle (&ctcpmux->retry) ||
              !grid_usock_isidle (&ctcpmux->usock) ||
              !grid_dns_isidle (&ctcpmux->dns))
            return;
        ctcpmux->state = GRID_CTCPMUX_STATE_IDLE;
        grid_fsm_stopped_noevent (&ctcpmux->fsm);
        grid_epbase_stopped (&ctcpmux->epbase);
        return;
    }

    grid_fsm_bad_state (ctcpmux->state, src, type);
}

static void grid_ctcpmux_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_ctcpmux *ctcpmux;
    struct grid_iovec iovec;

    ctcpmux = grid_cont (self, struct grid_ctcpmux, fsm);

    switch (ctcpmux->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  The state machine wasn't yet started.                                     */
/******************************************************************************/
    case GRID_CTCPMUX_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_ctcpmux_start_resolving (ctcpmux);
                return;
            default:
                grid_fsm_bad_action (ctcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (ctcpmux->state, src, type);
        }

/******************************************************************************/
/*  RESOLVING state.                                                          */
/*  Name of the host to connect to is being resolved to get an IP address.    */
/******************************************************************************/
    case GRID_CTCPMUX_STATE_RESOLVING:
        switch (src) {

        case GRID_CTCPMUX_SRC_DNS:
            switch (type) {
            case GRID_DNS_DONE:
                grid_dns_stop (&ctcpmux->dns);
                ctcpmux->state = GRID_CTCPMUX_STATE_STOPPING_DNS;
                return;
            default:
                grid_fsm_bad_action (ctcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (ctcpmux->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_DNS state.                                                       */
/*  dns object was asked to stop but it haven't stopped yet.                  */
/******************************************************************************/
    case GRID_CTCPMUX_STATE_STOPPING_DNS:
        switch (src) {

        case GRID_CTCPMUX_SRC_DNS:
            switch (type) {
            case GRID_DNS_STOPPED:
                if (ctcpmux->dns_result.error == 0) {
                    grid_ctcpmux_start_connecting (ctcpmux,
                        &ctcpmux->dns_result.addr,
                        ctcpmux->dns_result.addrlen);
                    return;
                }
                grid_backoff_start (&ctcpmux->retry);
                ctcpmux->state = GRID_CTCPMUX_STATE_WAITING;
                return;
            default:
                grid_fsm_bad_action (ctcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (ctcpmux->state, src, type);
        }

/******************************************************************************/
/*  CONNECTING state.                                                         */
/*  Non-blocking connect is under way.                                        */
/******************************************************************************/
    case GRID_CTCPMUX_STATE_CONNECTING:
        switch (src) {

        case GRID_CTCPMUX_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_CONNECTED:
                grid_epbase_stat_increment (&ctcpmux->epbase,
                    GRID_STAT_INPROGRESS_CONNECTIONS, -1);
                grid_epbase_stat_increment (&ctcpmux->epbase,
                    GRID_STAT_ESTABLISHED_CONNECTIONS, 1);
                grid_epbase_clear_error (&ctcpmux->epbase);
                iovec.iov_base = ctcpmux->buffer;
                iovec.iov_len = strlen (ctcpmux->buffer);
                grid_usock_send (&ctcpmux->usock, &iovec, 1);
                ctcpmux->state = GRID_CTCPMUX_STATE_SENDING_TCPMUXHDR;
                return;
            case GRID_USOCK_ERROR:
                grid_epbase_set_error (&ctcpmux->epbase,
                    grid_usock_geterrno (&ctcpmux->usock));
                grid_usock_stop (&ctcpmux->usock);
                ctcpmux->state = GRID_CTCPMUX_STATE_STOPPING_USOCK;
                grid_epbase_stat_increment (&ctcpmux->epbase,
                    GRID_STAT_INPROGRESS_CONNECTIONS, -1);
                grid_epbase_stat_increment (&ctcpmux->epbase,
                    GRID_STAT_CONNECT_ERRORS, 1);
                return;
            default:
                grid_fsm_bad_action (ctcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (ctcpmux->state, src, type);
        }

/******************************************************************************/
/* SENDING_TCPMUXHDR state.                                                   */
/******************************************************************************/
    case GRID_CTCPMUX_STATE_SENDING_TCPMUXHDR:
        switch (src) {
        case GRID_CTCPMUX_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SENT:
                grid_usock_recv (&ctcpmux->usock, ctcpmux->buffer, 3, NULL);
                ctcpmux->state = GRID_CTCPMUX_STATE_RECEIVING_TCPMUXHDR;
                return;
            case GRID_USOCK_ERROR:
                grid_epbase_set_error (&ctcpmux->epbase,
                    grid_usock_geterrno (&ctcpmux->usock));
                grid_usock_stop (&ctcpmux->usock);
                ctcpmux->state = GRID_CTCPMUX_STATE_STOPPING_USOCK;
                return;
            default:
                grid_fsm_bad_action (ctcpmux->state, src, type);
            }
        default:
            grid_fsm_bad_source (ctcpmux->state, src, type);
        }

/******************************************************************************/
/* RECEIVING_TCPMUXHDR state.                                                 */
/******************************************************************************/
    case GRID_CTCPMUX_STATE_RECEIVING_TCPMUXHDR:
        switch (src) {
        case GRID_CTCPMUX_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_RECEIVED:
                if (ctcpmux->buffer [0] == '+' &&
                      ctcpmux->buffer [1] == 0x0d &&
                      ctcpmux->buffer [2] == 0x0a) {
                    grid_stcpmux_start (&ctcpmux->stcpmux, &ctcpmux->usock);
                    ctcpmux->state = GRID_CTCPMUX_STATE_ACTIVE;
                    return;
                }
            case GRID_USOCK_ERROR:
                grid_epbase_set_error (&ctcpmux->epbase,
                    grid_usock_geterrno (&ctcpmux->usock));
                grid_usock_stop (&ctcpmux->usock);
                ctcpmux->state = GRID_CTCPMUX_STATE_STOPPING_USOCK;
                return;
            default:
                grid_fsm_bad_action (ctcpmux->state, src, type);
            }
        default:
            grid_fsm_bad_source (ctcpmux->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  Connection is established and handled by the stcpmux state machine.       */
/******************************************************************************/
    case GRID_CTCPMUX_STATE_ACTIVE:
        switch (src) {

        case GRID_CTCPMUX_SRC_STCPMUX:
            switch (type) {
            case GRID_STCPMUX_ERROR:
                grid_stcpmux_stop (&ctcpmux->stcpmux);
                ctcpmux->state = GRID_CTCPMUX_STATE_STOPPING_STCPMUX;
                grid_epbase_stat_increment (&ctcpmux->epbase,
                    GRID_STAT_BROKEN_CONNECTIONS, 1);
                return;
            default:
                grid_fsm_bad_action (ctcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (ctcpmux->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_STCPMUX state.                                                   */
/*  stcpmux object was asked to stop but it haven't stopped yet.              */
/******************************************************************************/
    case GRID_CTCPMUX_STATE_STOPPING_STCPMUX:
        switch (src) {

        case GRID_CTCPMUX_SRC_STCPMUX:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_STCPMUX_STOPPED:
                grid_usock_stop (&ctcpmux->usock);
                ctcpmux->state = GRID_CTCPMUX_STATE_STOPPING_USOCK;
                return;
            default:
                grid_fsm_bad_action (ctcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (ctcpmux->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_USOCK state.                                                     */
/*  usock object was asked to stop but it haven't stopped yet.                */
/******************************************************************************/
    case GRID_CTCPMUX_STATE_STOPPING_USOCK:
        switch (src) {

        case GRID_CTCPMUX_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_USOCK_STOPPED:
                grid_backoff_start (&ctcpmux->retry);
                ctcpmux->state = GRID_CTCPMUX_STATE_WAITING;
                return;
            default:
                grid_fsm_bad_action (ctcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (ctcpmux->state, src, type);
        }

/******************************************************************************/
/*  WAITING state.                                                            */
/*  Waiting before re-connection is attempted. This way we won't overload     */
/*  the system by continuous re-connection attemps.                           */
/******************************************************************************/
    case GRID_CTCPMUX_STATE_WAITING:
        switch (src) {

        case GRID_CTCPMUX_SRC_RECONNECT_TIMER:
            switch (type) {
            case GRID_BACKOFF_TIMEOUT:
                grid_backoff_stop (&ctcpmux->retry);
                ctcpmux->state = GRID_CTCPMUX_STATE_STOPPING_BACKOFF;
                return;
            default:
                grid_fsm_bad_action (ctcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (ctcpmux->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_BACKOFF state.                                                   */
/*  backoff object was asked to stop, but it haven't stopped yet.             */
/******************************************************************************/
    case GRID_CTCPMUX_STATE_STOPPING_BACKOFF:
        switch (src) {

        case GRID_CTCPMUX_SRC_RECONNECT_TIMER:
            switch (type) {
            case GRID_BACKOFF_STOPPED:
                grid_ctcpmux_start_resolving (ctcpmux);
                return;
            default:
                grid_fsm_bad_action (ctcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (ctcpmux->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (ctcpmux->state, src, type);
    }
}

/******************************************************************************/
/*  State machine actions.                                                    */
/******************************************************************************/

static void grid_ctcpmux_start_resolving (struct grid_ctcpmux *self)
{
    const char *addr;
    const char *begin;
    const char *end;
    int ipv4only;
    size_t ipv4onlylen;

    /*  Extract the hostname part from address string. */
    addr = grid_epbase_getaddr (&self->epbase);
    begin = strchr (addr, ';');
    if (!begin)
        begin = addr;
    else
        ++begin;
    end = strrchr (addr, ':');
    grid_assert (end);

    /*  Check whether IPv6 is to be used. */
    ipv4onlylen = sizeof (ipv4only);
    grid_epbase_getopt (&self->epbase, GRID_SOL_SOCKET, GRID_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    grid_assert (ipv4onlylen == sizeof (ipv4only));

    /*  TODO: Get the actual value of IPV4ONLY option. */
    grid_dns_start (&self->dns, begin, end - begin, ipv4only, &self->dns_result);

    self->state = GRID_CTCPMUX_STATE_RESOLVING;
}

static void grid_ctcpmux_start_connecting (struct grid_ctcpmux *self,
    struct sockaddr_storage *ss, size_t sslen)
{
    int rc;
    struct sockaddr_storage remote;
    size_t remotelen;
    struct sockaddr_storage local;
    size_t locallen;
    const char *addr;
    const char *end;
    const char *colon;
    const char *slash;
    const char *semicolon;
    uint16_t port;
    int ipv4only;
    size_t ipv4onlylen;
    int val;
    size_t sz;

    /*  Create IP address from the address string. */
    addr = grid_epbase_getaddr (&self->epbase);
    memset (&remote, 0, sizeof (remote));

    semicolon = strchr (addr, ';');
    colon = strchr ((semicolon ? semicolon : addr) + 1, ':');
    slash = strchr (colon + 1, '/');
    end = addr + strlen (addr);

    /*  Parse the port. */
    rc = grid_port_resolve (colon + 1, slash - colon - 1);
    errnum_assert (rc > 0, -rc);
    port = rc;

    /*  Copy the URL to the buffer. Append it by CRLF. */
    sz = end - (slash + 1);
    memcpy (self->buffer, slash + 1, sz);
    self->buffer [sz] = 0x0d;
    self->buffer [sz + 1] = 0x0a;
    self->buffer [sz + 2] = 0;

    /*  Check whether IPv6 is to be used. */
    ipv4onlylen = sizeof (ipv4only);
    grid_epbase_getopt (&self->epbase, GRID_SOL_SOCKET, GRID_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    grid_assert (ipv4onlylen == sizeof (ipv4only));

    /*  Parse the local address, if any. */
    memset (&local, 0, sizeof (local));
    if (semicolon)
        rc = grid_iface_resolve (addr, semicolon - addr, ipv4only,
            &local, &locallen);
    else
        rc = grid_iface_resolve ("*", 1, ipv4only, &local, &locallen);
    if (grid_slow (rc < 0)) {
        grid_backoff_start (&self->retry);
        self->state = GRID_CTCPMUX_STATE_WAITING;
        return;
    }

    /*  Combine the remote address and the port. */
    remote = *ss;
    remotelen = sslen;
    if (remote.ss_family == AF_INET)
        ((struct sockaddr_in*) &remote)->sin_port = htons (port);
    else if (remote.ss_family == AF_INET6)
        ((struct sockaddr_in6*) &remote)->sin6_port = htons (port);
    else
        grid_assert (0);

    /*  Try to start the underlying socket. */
    rc = grid_usock_start (&self->usock, remote.ss_family, SOCK_STREAM, 0);
    if (grid_slow (rc < 0)) {
        grid_backoff_start (&self->retry);
        self->state = GRID_CTCPMUX_STATE_WAITING;
        return;
    }

    /*  Set the relevant socket options. */
    sz = sizeof (val);
    grid_epbase_getopt (&self->epbase, GRID_SOL_SOCKET, GRID_SNDBUF, &val, &sz);
    grid_assert (sz == sizeof (val));
    grid_usock_setsockopt (&self->usock, SOL_SOCKET, SO_SNDBUF,
        &val, sizeof (val));
    sz = sizeof (val);
    grid_epbase_getopt (&self->epbase, GRID_SOL_SOCKET, GRID_RCVBUF, &val, &sz);
    grid_assert (sz == sizeof (val));
    grid_usock_setsockopt (&self->usock, SOL_SOCKET, SO_RCVBUF,
        &val, sizeof (val));

    /*  Bind the socket to the local network interface. */
    rc = grid_usock_bind (&self->usock, (struct sockaddr*) &local, locallen);
    if (grid_slow (rc != 0)) {
        grid_backoff_start (&self->retry);
        self->state = GRID_CTCPMUX_STATE_WAITING;
        return;
    }

    /*  Start connecting. */
    grid_usock_connect (&self->usock, (struct sockaddr*) &remote, remotelen);
    self->state = GRID_CTCPMUX_STATE_CONNECTING;
    grid_epbase_stat_increment (&self->epbase,
        GRID_STAT_INPROGRESS_CONNECTIONS, 1);
}

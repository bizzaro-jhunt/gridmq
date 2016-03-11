/*
    Copyright (c) 2012-2013 250bpm s.r.o.  All rights reserved.
    Copyright (c) 2014 Wirebird Labs LLC.  All rights reserved.
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

#include "cws.h"
#include "sws.h"

#include "../../ws.h"

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

#define GRID_CWS_STATE_IDLE 1
#define GRID_CWS_STATE_RESOLVING 2
#define GRID_CWS_STATE_STOPPING_DNS 3
#define GRID_CWS_STATE_CONNECTING 4
#define GRID_CWS_STATE_ACTIVE 5
#define GRID_CWS_STATE_STOPPING_SWS 6
#define GRID_CWS_STATE_STOPPING_USOCK 7
#define GRID_CWS_STATE_WAITING 8
#define GRID_CWS_STATE_STOPPING_BACKOFF 9
#define GRID_CWS_STATE_STOPPING_SWS_FINAL 10
#define GRID_CWS_STATE_STOPPING 11

#define GRID_CWS_SRC_USOCK 1
#define GRID_CWS_SRC_RECONNECT_TIMER 2
#define GRID_CWS_SRC_DNS 3
#define GRID_CWS_SRC_SWS 4

struct grid_cws {

    /*  The state machine. */
    struct grid_fsm fsm;
    int state;

    /*  This object is a specific type of endpoint.
        Thus it is derived from epbase. */
    struct grid_epbase epbase;

    /*  The underlying WS socket. */
    struct grid_usock usock;

    /*  Used to wait before retrying to connect. */
    struct grid_backoff retry;

    /*  Defines message validation and framing. */
    uint8_t msg_type;

    /*  State machine that handles the active part of the connection
        lifetime. */
    struct grid_sws sws;

    /*  Parsed parts of the connection URI. */
    struct grid_chunkref resource;
    struct grid_chunkref remote_host;
    struct grid_chunkref nic;
    int remote_port;
    int remote_hostname_len;

    /*  If a close handshake is performed, this flag signals to not
        begin automatic reconnect retries. */
    int peer_gone;

    /*  DNS resolver used to convert textual address into actual IP address
        along with the variable to hold the result. */
    struct grid_dns dns;
    struct grid_dns_result dns_result;
};

/*  grid_epbase virtual interface implementation. */
static void grid_cws_stop (struct grid_epbase *self);
static void grid_cws_destroy (struct grid_epbase *self);
const struct grid_epbase_vfptr grid_cws_epbase_vfptr = {
    grid_cws_stop,
    grid_cws_destroy
};

/*  Private functions. */
static void grid_cws_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_cws_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_cws_start_resolving (struct grid_cws *self);
static void grid_cws_start_connecting (struct grid_cws *self,
    struct sockaddr_storage *ss, size_t sslen);

int grid_cws_create (void *hint, struct grid_epbase **epbase)
{
    int rc;
    const char *addr;
    size_t addrlen;
    const char *semicolon;
    const char *hostname;
    size_t hostlen;
    const char *colon;
    const char *slash;
    const char *resource;
    size_t resourcelen;
    struct sockaddr_storage ss;
    size_t sslen;
    int ipv4only;
    size_t ipv4onlylen;
    struct grid_cws *self;
    int reconnect_ivl;
    int reconnect_ivl_max;
    int msg_type;
    size_t sz;

    /*  Allocate the new endpoint object. */
    self = grid_alloc (sizeof (struct grid_cws), "cws");
    alloc_assert (self);

    /*  Initalise the endpoint. */
    grid_epbase_init (&self->epbase, &grid_cws_epbase_vfptr, hint);

    /*  Check whether IPv6 is to be used. */
    ipv4onlylen = sizeof (ipv4only);
    grid_epbase_getopt (&self->epbase, GRID_SOL_SOCKET, GRID_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    grid_assert (ipv4onlylen == sizeof (ipv4only));

    /*  Start parsing the address. */
    addr = grid_epbase_getaddr (&self->epbase);
    addrlen = strlen (addr);
    semicolon = strchr (addr, ';');
    hostname = semicolon ? semicolon + 1 : addr;
    colon = strrchr (addr, ':');
    slash = colon ? strchr (colon, '/') : strchr (addr, '/');
    resource = slash ? slash : addr + addrlen;
    self->remote_hostname_len = colon ? colon - hostname : resource - hostname;
    
    /*  Host contains both hostname and port. */
    hostlen = resource - hostname;

    /*  Parse the port; assume port 80 if not explicitly declared. */
    if (grid_slow (colon != NULL)) {
        rc = grid_port_resolve (colon + 1, resource - colon - 1);
        if (grid_slow (rc < 0)) {
            grid_epbase_term (&self->epbase);
            return -EINVAL;
        }
        self->remote_port = rc;
    }
    else {
        self->remote_port = 80;
    }

    /*  Check whether the host portion of the address is either a literal
        or a valid hostname. */
    if (grid_dns_check_hostname (hostname, self->remote_hostname_len) < 0 &&
          grid_literal_resolve (hostname, self->remote_hostname_len, ipv4only,
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

    /*  At this point, the address is valid, so begin allocating resources. */
    grid_chunkref_init (&self->remote_host, hostlen + 1);
    memcpy (grid_chunkref_data (&self->remote_host), hostname, hostlen);
    ((uint8_t *) grid_chunkref_data (&self->remote_host)) [hostlen] = '\0';

    if (semicolon) {
        grid_chunkref_init (&self->nic, semicolon - addr);
        memcpy (grid_chunkref_data (&self->nic),
            addr, semicolon - addr);
    }
    else {
        grid_chunkref_init (&self->nic, 1);
        memcpy (grid_chunkref_data (&self->nic), "*", 1);
    }

    /*  The requested resource is used in opening handshake. */
    resourcelen = strlen (resource);
    if (resourcelen) {
        grid_chunkref_init (&self->resource, resourcelen + 1);
        strncpy (grid_chunkref_data (&self->resource),
            resource, resourcelen + 1);
    }
    else {
        /*  No resource specified, so allocate base path. */
        grid_chunkref_init (&self->resource, 2);
        strncpy (grid_chunkref_data (&self->resource), "/", 2);
    }

    /*  Initialise the structure. */
    grid_fsm_init_root (&self->fsm, grid_cws_handler, grid_cws_shutdown,
        grid_epbase_getctx (&self->epbase));
    self->state = GRID_CWS_STATE_IDLE;
    grid_usock_init (&self->usock, GRID_CWS_SRC_USOCK, &self->fsm);
    
    sz = sizeof (msg_type);
    grid_epbase_getopt (&self->epbase, GRID_WS, GRID_WS_MSG_TYPE,
        &msg_type, &sz);
    grid_assert (sz == sizeof (msg_type));
    self->msg_type = (uint8_t) msg_type;

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
    grid_backoff_init (&self->retry, GRID_CWS_SRC_RECONNECT_TIMER,
        reconnect_ivl, reconnect_ivl_max, &self->fsm);

    grid_sws_init (&self->sws, GRID_CWS_SRC_SWS, &self->epbase, &self->fsm);
    grid_dns_init (&self->dns, GRID_CWS_SRC_DNS, &self->fsm);

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);

    /*  Return the base class as an out parameter. */
    *epbase = &self->epbase;

    return 0;
}

static void grid_cws_stop (struct grid_epbase *self)
{
    struct grid_cws *cws;

    cws = grid_cont (self, struct grid_cws, epbase);

    grid_fsm_stop (&cws->fsm);
}

static void grid_cws_destroy (struct grid_epbase *self)
{
    struct grid_cws *cws;

    cws = grid_cont (self, struct grid_cws, epbase);

    grid_chunkref_term (&cws->resource);
    grid_chunkref_term (&cws->remote_host);
    grid_chunkref_term (&cws->nic);
    grid_dns_term (&cws->dns);
    grid_sws_term (&cws->sws);
    grid_backoff_term (&cws->retry);
    grid_usock_term (&cws->usock);
    grid_fsm_term (&cws->fsm);
    grid_epbase_term (&cws->epbase);

    grid_free (cws);
}

static void grid_cws_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_cws *cws;

    cws = grid_cont (self, struct grid_cws, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        if (!grid_sws_isidle (&cws->sws)) {
            grid_epbase_stat_increment (&cws->epbase,
                GRID_STAT_DROPPED_CONNECTIONS, 1);
            grid_sws_stop (&cws->sws);
        }
        cws->state = GRID_CWS_STATE_STOPPING_SWS_FINAL;
    }
    if (grid_slow (cws->state == GRID_CWS_STATE_STOPPING_SWS_FINAL)) {
        if (!grid_sws_isidle (&cws->sws))
            return;
        grid_backoff_stop (&cws->retry);
        grid_usock_stop (&cws->usock);
        grid_dns_stop (&cws->dns);
        cws->state = GRID_CWS_STATE_STOPPING;
    }
    if (grid_slow (cws->state == GRID_CWS_STATE_STOPPING)) {
        if (!grid_backoff_isidle (&cws->retry) ||
              !grid_usock_isidle (&cws->usock) ||
              !grid_dns_isidle (&cws->dns))
            return;
        cws->state = GRID_CWS_STATE_IDLE;
        grid_fsm_stopped_noevent (&cws->fsm);
        grid_epbase_stopped (&cws->epbase);
        return;
    }

    grid_fsm_bad_state (cws->state, src, type);
}

static void grid_cws_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_cws *cws;

    cws = grid_cont (self, struct grid_cws, fsm);

    switch (cws->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  The state machine wasn't yet started.                                     */
/******************************************************************************/
    case GRID_CWS_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_cws_start_resolving (cws);
                return;
            default:
                grid_fsm_bad_action (cws->state, src, type);
            }

        default:
            grid_fsm_bad_source (cws->state, src, type);
        }

/******************************************************************************/
/*  RESOLVING state.                                                          */
/*  Name of the host to connect to is being resolved to get an IP address.    */
/******************************************************************************/
    case GRID_CWS_STATE_RESOLVING:
        switch (src) {

        case GRID_CWS_SRC_DNS:
            switch (type) {
            case GRID_DNS_DONE:
                grid_dns_stop (&cws->dns);
                cws->state = GRID_CWS_STATE_STOPPING_DNS;
                return;
            default:
                grid_fsm_bad_action (cws->state, src, type);
            }

        default:
            grid_fsm_bad_source (cws->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_DNS state.                                                       */
/*  dns object was asked to stop but it haven't stopped yet.                  */
/******************************************************************************/
    case GRID_CWS_STATE_STOPPING_DNS:
        switch (src) {

        case GRID_CWS_SRC_DNS:
            switch (type) {
            case GRID_DNS_STOPPED:
                if (cws->dns_result.error == 0) {
                    grid_cws_start_connecting (cws, &cws->dns_result.addr,
                        cws->dns_result.addrlen);
                    return;
                }
                grid_backoff_start (&cws->retry);
                cws->state = GRID_CWS_STATE_WAITING;
                return;
            default:
                grid_fsm_bad_action (cws->state, src, type);
            }

        default:
            grid_fsm_bad_source (cws->state, src, type);
        }

/******************************************************************************/
/*  CONNECTING state.                                                         */
/*  Non-blocking connect is under way.                                        */
/******************************************************************************/
    case GRID_CWS_STATE_CONNECTING:
        switch (src) {

        case GRID_CWS_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_CONNECTED:
                grid_sws_start (&cws->sws, &cws->usock, GRID_WS_CLIENT,
                    grid_chunkref_data (&cws->resource),
                    grid_chunkref_data (&cws->remote_host), cws->msg_type);
                cws->state = GRID_CWS_STATE_ACTIVE;
                cws->peer_gone = 0;
                grid_epbase_stat_increment (&cws->epbase,
                    GRID_STAT_INPROGRESS_CONNECTIONS, -1);
                grid_epbase_stat_increment (&cws->epbase,
                    GRID_STAT_ESTABLISHED_CONNECTIONS, 1);
                grid_epbase_clear_error (&cws->epbase);
                return;
            case GRID_USOCK_ERROR:
                grid_epbase_set_error (&cws->epbase,
                    grid_usock_geterrno (&cws->usock));
                grid_usock_stop (&cws->usock);
                cws->state = GRID_CWS_STATE_STOPPING_USOCK;
                grid_epbase_stat_increment (&cws->epbase,
                    GRID_STAT_INPROGRESS_CONNECTIONS, -1);
                grid_epbase_stat_increment (&cws->epbase,
                    GRID_STAT_CONNECT_ERRORS, 1);
                return;
            default:
                grid_fsm_bad_action (cws->state, src, type);
            }

        default:
            grid_fsm_bad_source (cws->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  Connection is established and handled by the sws state machine.           */
/******************************************************************************/
    case GRID_CWS_STATE_ACTIVE:
        switch (src) {

        case GRID_CWS_SRC_SWS:
            switch (type) {
            case GRID_SWS_RETURN_CLOSE_HANDSHAKE:
                /*  Peer closed connection without intention to reconnect, or
                    local endpoint failed remote because of invalid data. */
                grid_sws_stop (&cws->sws);
                cws->state = GRID_CWS_STATE_STOPPING_SWS;
                cws->peer_gone = 1;
                return;
            case GRID_SWS_RETURN_ERROR:
                grid_sws_stop (&cws->sws);
                cws->state = GRID_CWS_STATE_STOPPING_SWS;
                grid_epbase_stat_increment (&cws->epbase,
                    GRID_STAT_BROKEN_CONNECTIONS, 1);
                return;
            default:
                grid_fsm_bad_action (cws->state, src, type);
            }

        default:
            grid_fsm_bad_source (cws->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_SWS state.                                                       */
/*  sws object was asked to stop but it haven't stopped yet.                  */
/******************************************************************************/
    case GRID_CWS_STATE_STOPPING_SWS:
        switch (src) {

        case GRID_CWS_SRC_SWS:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_SWS_RETURN_STOPPED:
                grid_usock_stop (&cws->usock);
                cws->state = GRID_CWS_STATE_STOPPING_USOCK;
                return;
            default:
                grid_fsm_bad_action (cws->state, src, type);
            }

        default:
            grid_fsm_bad_source (cws->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_USOCK state.                                                     */
/*  usock object was asked to stop but it haven't stopped yet.                */
/******************************************************************************/
    case GRID_CWS_STATE_STOPPING_USOCK:
        switch (src) {

        case GRID_CWS_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_USOCK_STOPPED:
                /*  If the peer has confirmed itself gone with a Closing
                    Handshake, or if the local endpoint failed the remote,
                    don't try to reconnect. */
                if (cws->peer_gone) {
                    /*  It is expected that the application detects this and
                        prunes the connection with grid_shutdown. */
                }
                else {
                    grid_backoff_start (&cws->retry);
                    cws->state = GRID_CWS_STATE_WAITING;
                }
                return;
            default:
                grid_fsm_bad_action (cws->state, src, type);
            }

        default:
            grid_fsm_bad_source (cws->state, src, type);
        }

/******************************************************************************/
/*  WAITING state.                                                            */
/*  Waiting before re-connection is attempted. This way we won't overload     */
/*  the system by continuous re-connection attemps.                           */
/******************************************************************************/
    case GRID_CWS_STATE_WAITING:
        switch (src) {

        case GRID_CWS_SRC_RECONNECT_TIMER:
            switch (type) {
            case GRID_BACKOFF_TIMEOUT:
                grid_backoff_stop (&cws->retry);
                cws->state = GRID_CWS_STATE_STOPPING_BACKOFF;
                return;
            default:
                grid_fsm_bad_action (cws->state, src, type);
            }

        default:
            grid_fsm_bad_source (cws->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_BACKOFF state.                                                   */
/*  backoff object was asked to stop, but it haven't stopped yet.             */
/******************************************************************************/
    case GRID_CWS_STATE_STOPPING_BACKOFF:
        switch (src) {

        case GRID_CWS_SRC_RECONNECT_TIMER:
            switch (type) {
            case GRID_BACKOFF_STOPPED:
                grid_cws_start_resolving (cws);
                return;
            default:
                grid_fsm_bad_action (cws->state, src, type);
            }

        default:
            grid_fsm_bad_source (cws->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (cws->state, src, type);
    }
}

/******************************************************************************/
/*  State machine actions.                                                    */
/******************************************************************************/

static void grid_cws_start_resolving (struct grid_cws *self)
{
    int ipv4only;
    size_t ipv4onlylen;
    char *host;

    /*  Check whether IPv6 is to be used. */
    ipv4onlylen = sizeof (ipv4only);
    grid_epbase_getopt (&self->epbase, GRID_SOL_SOCKET, GRID_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    grid_assert (ipv4onlylen == sizeof (ipv4only));

    host = grid_chunkref_data (&self->remote_host);
    grid_assert (strlen (host) > 0);

    grid_dns_start (&self->dns, host, self->remote_hostname_len, ipv4only,
        &self->dns_result);

    self->state = GRID_CWS_STATE_RESOLVING;
}

static void grid_cws_start_connecting (struct grid_cws *self,
    struct sockaddr_storage *ss, size_t sslen)
{
    int rc;
    struct sockaddr_storage remote;
    size_t remotelen;
    struct sockaddr_storage local;
    size_t locallen;
    int ipv4only;
    size_t ipv4onlylen;
    int val;
    size_t sz;

    memset (&remote, 0, sizeof (remote));
    memset (&local, 0, sizeof (local));

    /*  Check whether IPv6 is to be used. */
    ipv4onlylen = sizeof (ipv4only);
    grid_epbase_getopt (&self->epbase, GRID_SOL_SOCKET, GRID_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    grid_assert (ipv4onlylen == sizeof (ipv4only));

    rc = grid_iface_resolve (grid_chunkref_data (&self->nic),
    grid_chunkref_size (&self->nic), ipv4only, &local, &locallen);

    if (grid_slow (rc < 0)) {
        grid_backoff_start (&self->retry);
        self->state = GRID_CWS_STATE_WAITING;
        return;
    }

    /*  Combine the remote address and the port. */
    remote = *ss;
    remotelen = sslen;
    if (remote.ss_family == AF_INET)
        ((struct sockaddr_in*) &remote)->sin_port = htons (self->remote_port);
    else if (remote.ss_family == AF_INET6)
        ((struct sockaddr_in6*) &remote)->sin6_port = htons (self->remote_port);
    else
        grid_assert (0);

    /*  Try to start the underlying socket. */
    rc = grid_usock_start (&self->usock, remote.ss_family, SOCK_STREAM, 0);
    if (grid_slow (rc < 0)) {
        grid_backoff_start (&self->retry);
        self->state = GRID_CWS_STATE_WAITING;
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
    errnum_assert (rc == 0, -rc);

    /*  Start connecting. */
    grid_usock_connect (&self->usock, (struct sockaddr*) &remote, remotelen);
    self->state = GRID_CWS_STATE_CONNECTING;
    grid_epbase_stat_increment (&self->epbase,
        GRID_STAT_INPROGRESS_CONNECTIONS, 1);
}

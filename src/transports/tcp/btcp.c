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

#include "btcp.h"
#include "atcp.h"

#include "../utils/port.h"
#include "../utils/iface.h"

#include "../../aio/fsm.h"
#include "../../aio/usock.h"

#include "../utils/backoff.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"
#include "../../utils/fast.h"
#include "../../utils/int.h"

#include <string.h>
#include <unistd.h>
#include <netinet/in.h>

/*  The backlog is set relatively high so that there are not too many failed
    connection attemps during re-connection storms. */
#define GRID_BTCP_BACKLOG 100

#define GRID_BTCP_STATE_IDLE 1
#define GRID_BTCP_STATE_ACTIVE 2
#define GRID_BTCP_STATE_STOPPING_ATCP 3
#define GRID_BTCP_STATE_STOPPING_USOCK 4
#define GRID_BTCP_STATE_STOPPING_ATCPS 5
#define GRID_BTCP_STATE_LISTENING 6
#define GRID_BTCP_STATE_WAITING 7
#define GRID_BTCP_STATE_CLOSING 8
#define GRID_BTCP_STATE_STOPPING_BACKOFF 9

#define GRID_BTCP_SRC_USOCK 1
#define GRID_BTCP_SRC_ATCP 2
#define GRID_BTCP_SRC_RECONNECT_TIMER 3

struct grid_btcp {

    /*  The state machine. */
    struct grid_fsm fsm;
    int state;

    /*  This object is a specific type of endpoint.
        Thus it is derived from epbase. */
    struct grid_epbase epbase;

    /*  The underlying listening TCP socket. */
    struct grid_usock usock;

    /*  The connection being accepted at the moment. */
    struct grid_atcp *atcp;

    /*  List of accepted connections. */
    struct grid_list atcps;

    /*  Used to wait before retrying to connect. */
    struct grid_backoff retry;
};

/*  grid_epbase virtual interface implementation. */
static void grid_btcp_stop (struct grid_epbase *self);
static void grid_btcp_destroy (struct grid_epbase *self);
const struct grid_epbase_vfptr grid_btcp_epbase_vfptr = {
    grid_btcp_stop,
    grid_btcp_destroy
};

/*  Private functions. */
static void grid_btcp_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_btcp_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_btcp_start_listening (struct grid_btcp *self);
static void grid_btcp_start_accepting (struct grid_btcp *self);

int grid_btcp_create (void *hint, struct grid_epbase **epbase)
{
    int rc;
    struct grid_btcp *self;
    const char *addr;
    const char *end;
    const char *pos;
    struct sockaddr_storage ss;
    size_t sslen;
    int ipv4only;
    size_t ipv4onlylen;
    int reconnect_ivl;
    int reconnect_ivl_max;
    size_t sz;

    /*  Allocate the new endpoint object. */
    self = grid_alloc (sizeof (struct grid_btcp), "btcp");
    alloc_assert (self);

    /*  Initalise the epbase. */
    grid_epbase_init (&self->epbase, &grid_btcp_epbase_vfptr, hint);
    addr = grid_epbase_getaddr (&self->epbase);

    /*  Parse the port. */
    end = addr + strlen (addr);
    pos = strrchr (addr, ':');
    if (grid_slow (!pos)) {
        grid_epbase_term (&self->epbase);
        return -EINVAL;
    }
    ++pos;
    rc = grid_port_resolve (pos, end - pos);
    if (grid_slow (rc < 0)) {
        grid_epbase_term (&self->epbase);
        return -EINVAL;
    }

    /*  Check whether IPv6 is to be used. */
    ipv4onlylen = sizeof (ipv4only);
    grid_epbase_getopt (&self->epbase, GRID_SOL_SOCKET, GRID_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    grid_assert (ipv4onlylen == sizeof (ipv4only));

    /*  Parse the address. */
    rc = grid_iface_resolve (addr, pos - addr - 1, ipv4only, &ss, &sslen);
    if (grid_slow (rc < 0)) {
        grid_epbase_term (&self->epbase);
        return -ENODEV;
    }

    /*  Initialise the structure. */
    grid_fsm_init_root (&self->fsm, grid_btcp_handler, grid_btcp_shutdown,
        grid_epbase_getctx (&self->epbase));
    self->state = GRID_BTCP_STATE_IDLE;
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
    grid_backoff_init (&self->retry, GRID_BTCP_SRC_RECONNECT_TIMER,
        reconnect_ivl, reconnect_ivl_max, &self->fsm);
    grid_usock_init (&self->usock, GRID_BTCP_SRC_USOCK, &self->fsm);
    self->atcp = NULL;
    grid_list_init (&self->atcps);

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);

    /*  Return the base class as an out parameter. */
    *epbase = &self->epbase;

    return 0;
}

static void grid_btcp_stop (struct grid_epbase *self)
{
    struct grid_btcp *btcp;

    btcp = grid_cont (self, struct grid_btcp, epbase);

    grid_fsm_stop (&btcp->fsm);
}

static void grid_btcp_destroy (struct grid_epbase *self)
{
    struct grid_btcp *btcp;

    btcp = grid_cont (self, struct grid_btcp, epbase);

    grid_assert_state (btcp, GRID_BTCP_STATE_IDLE);
    grid_list_term (&btcp->atcps);
    grid_assert (btcp->atcp == NULL);
    grid_usock_term (&btcp->usock);
    grid_backoff_term (&btcp->retry);
    grid_epbase_term (&btcp->epbase);
    grid_fsm_term (&btcp->fsm);

    grid_free (btcp);
}

static void grid_btcp_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    struct grid_btcp *btcp;
    struct grid_list_item *it;
    struct grid_atcp *atcp;

    btcp = grid_cont (self, struct grid_btcp, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        grid_backoff_stop (&btcp->retry);
        if (btcp->atcp) {
            grid_atcp_stop (btcp->atcp);
            btcp->state = GRID_BTCP_STATE_STOPPING_ATCP;
        }
        else {
            btcp->state = GRID_BTCP_STATE_STOPPING_USOCK;
        }
    }
    if (grid_slow (btcp->state == GRID_BTCP_STATE_STOPPING_ATCP)) {
        if (!grid_atcp_isidle (btcp->atcp))
            return;
        grid_atcp_term (btcp->atcp);
        grid_free (btcp->atcp);
        btcp->atcp = NULL;
        grid_usock_stop (&btcp->usock);
        btcp->state = GRID_BTCP_STATE_STOPPING_USOCK;
    }
    if (grid_slow (btcp->state == GRID_BTCP_STATE_STOPPING_USOCK)) {
       if (!grid_usock_isidle (&btcp->usock) ||
             !grid_backoff_isidle (&btcp->retry))
            return;
        for (it = grid_list_begin (&btcp->atcps);
              it != grid_list_end (&btcp->atcps);
              it = grid_list_next (&btcp->atcps, it)) {
            atcp = grid_cont (it, struct grid_atcp, item);
            grid_atcp_stop (atcp);
        }
        btcp->state = GRID_BTCP_STATE_STOPPING_ATCPS;
        goto atcps_stopping;
    }
    if (grid_slow (btcp->state == GRID_BTCP_STATE_STOPPING_ATCPS)) {
        grid_assert (src == GRID_BTCP_SRC_ATCP && type == GRID_ATCP_STOPPED);
        atcp = (struct grid_atcp *) srcptr;
        grid_list_erase (&btcp->atcps, &atcp->item);
        grid_atcp_term (atcp);
        grid_free (atcp);

        /*  If there are no more atcp state machines, we can stop the whole
            btcp object. */
atcps_stopping:
        if (grid_list_empty (&btcp->atcps)) {
            btcp->state = GRID_BTCP_STATE_IDLE;
            grid_fsm_stopped_noevent (&btcp->fsm);
            grid_epbase_stopped (&btcp->epbase);
            return;
        }

        return;
    }

    grid_fsm_bad_action(btcp->state, src, type);
}

static void grid_btcp_handler (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    struct grid_btcp *btcp;
    struct grid_atcp *atcp;

    btcp = grid_cont (self, struct grid_btcp, fsm);

    switch (btcp->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case GRID_BTCP_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_btcp_start_listening (btcp);
                return;
            default:
                grid_fsm_bad_action (btcp->state, src, type);
            }

        default:
            grid_fsm_bad_source (btcp->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  The execution is yielded to the atcp state machine in this state.         */
/******************************************************************************/
    case GRID_BTCP_STATE_ACTIVE:
        if (srcptr == btcp->atcp) {
            switch (type) {
            case GRID_ATCP_ACCEPTED:

                /*  Move the newly created connection to the list of existing
                    connections. */
                grid_list_insert (&btcp->atcps, &btcp->atcp->item,
                    grid_list_end (&btcp->atcps));
                btcp->atcp = NULL;

                /*  Start waiting for a new incoming connection. */
                grid_btcp_start_accepting (btcp);

                return;

            default:
                grid_fsm_bad_action (btcp->state, src, type);
            }
        }

        /*  For all remaining events we'll assume they are coming from one
            of remaining child atcp objects. */
        grid_assert (src == GRID_BTCP_SRC_ATCP);
        atcp = (struct grid_atcp*) srcptr;
        switch (type) {
        case GRID_ATCP_ERROR:
            grid_atcp_stop (atcp);
            return;
        case GRID_ATCP_STOPPED:
            grid_list_erase (&btcp->atcps, &atcp->item);
            grid_atcp_term (atcp);
            grid_free (atcp);
            return;
        default:
            grid_fsm_bad_action (btcp->state, src, type);
        }

/******************************************************************************/
/*  CLOSING_USOCK state.                                                     */
/*  usock object was asked to stop but it haven't stopped yet.                */
/******************************************************************************/
    case GRID_BTCP_STATE_CLOSING:
        switch (src) {

        case GRID_BTCP_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_USOCK_STOPPED:
                grid_backoff_start (&btcp->retry);
                btcp->state = GRID_BTCP_STATE_WAITING;
                return;
            default:
                grid_fsm_bad_action (btcp->state, src, type);
            }

        default:
            grid_fsm_bad_source (btcp->state, src, type);
        }

/******************************************************************************/
/*  WAITING state.                                                            */
/*  Waiting before re-bind is attempted. This way we won't overload           */
/*  the system by continuous re-bind attemps.                                 */
/******************************************************************************/
    case GRID_BTCP_STATE_WAITING:
        switch (src) {

        case GRID_BTCP_SRC_RECONNECT_TIMER:
            switch (type) {
            case GRID_BACKOFF_TIMEOUT:
                grid_backoff_stop (&btcp->retry);
                btcp->state = GRID_BTCP_STATE_STOPPING_BACKOFF;
                return;
            default:
                grid_fsm_bad_action (btcp->state, src, type);
            }

        default:
            grid_fsm_bad_source (btcp->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_BACKOFF state.                                                   */
/*  backoff object was asked to stop, but it haven't stopped yet.             */
/******************************************************************************/
    case GRID_BTCP_STATE_STOPPING_BACKOFF:
        switch (src) {

        case GRID_BTCP_SRC_RECONNECT_TIMER:
            switch (type) {
            case GRID_BACKOFF_STOPPED:
                grid_btcp_start_listening (btcp);
                return;
            default:
                grid_fsm_bad_action (btcp->state, src, type);
            }

        default:
            grid_fsm_bad_source (btcp->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (btcp->state, src, type);
    }
}

/******************************************************************************/
/*  State machine actions.                                                    */
/******************************************************************************/

static void grid_btcp_start_listening (struct grid_btcp *self)
{
    int rc;
    struct sockaddr_storage ss;
    size_t sslen;
    int ipv4only;
    size_t ipv4onlylen;
    const char *addr;
    const char *end;
    const char *pos;
    uint16_t port;

    /*  First, resolve the IP address. */
    addr = grid_epbase_getaddr (&self->epbase);
    memset (&ss, 0, sizeof (ss));

    /*  Parse the port. */
    end = addr + strlen (addr);
    pos = strrchr (addr, ':');
    grid_assert (pos);
    ++pos;
    rc = grid_port_resolve (pos, end - pos);
    grid_assert (rc >= 0);
    port = rc;

    /*  Parse the address. */
    ipv4onlylen = sizeof (ipv4only);
    grid_epbase_getopt (&self->epbase, GRID_SOL_SOCKET, GRID_IPV4ONLY,
        &ipv4only, &ipv4onlylen);
    grid_assert (ipv4onlylen == sizeof (ipv4only));
    rc = grid_iface_resolve (addr, pos - addr - 1, ipv4only, &ss, &sslen);
    errnum_assert (rc == 0, -rc);

    /*  Combine the port and the address. */
    if (ss.ss_family == AF_INET) {
        ((struct sockaddr_in*) &ss)->sin_port = htons (port);
        sslen = sizeof (struct sockaddr_in);
    }
    else if (ss.ss_family == AF_INET6) {
        ((struct sockaddr_in6*) &ss)->sin6_port = htons (port);
        sslen = sizeof (struct sockaddr_in6);
    }
    else
        grid_assert (0);

    /*  Start listening for incoming connections. */
    rc = grid_usock_start (&self->usock, ss.ss_family, SOCK_STREAM, 0);
    if (grid_slow (rc < 0)) {
        grid_backoff_start (&self->retry);
        self->state = GRID_BTCP_STATE_WAITING;
        return;
    }

    rc = grid_usock_bind (&self->usock, (struct sockaddr*) &ss, (size_t) sslen);
    if (grid_slow (rc < 0)) {
        grid_usock_stop (&self->usock);
        self->state = GRID_BTCP_STATE_CLOSING;
        return;
    }

    rc = grid_usock_listen (&self->usock, GRID_BTCP_BACKLOG);
    if (grid_slow (rc < 0)) {
        grid_usock_stop (&self->usock);
        self->state = GRID_BTCP_STATE_CLOSING;
        return;
    }
    grid_btcp_start_accepting(self);
    self->state = GRID_BTCP_STATE_ACTIVE;
}

static void grid_btcp_start_accepting (struct grid_btcp *self)
{
    grid_assert (self->atcp == NULL);

    /*  Allocate new atcp state machine. */
    self->atcp = grid_alloc (sizeof (struct grid_atcp), "atcp");
    alloc_assert (self->atcp);
    grid_atcp_init (self->atcp, GRID_BTCP_SRC_ATCP, &self->epbase, &self->fsm);

    /*  Start waiting for a new incoming connection. */
    grid_atcp_start (self->atcp, &self->usock);
}


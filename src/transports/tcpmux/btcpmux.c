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

#include "btcpmux.h"
#include "atcpmux.h"

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
#include <sys/un.h>
#include <arpa/inet.h>

/*  The backlog is set relatively high so that there are not too many failed
    connection attemps during re-connection storms. */
#define GRID_BTCPMUX_BACKLOG 100

#define GRID_BTCPMUX_STATE_IDLE 1
#define GRID_BTCPMUX_STATE_CONNECTING 2
#define GRID_BTCPMUX_STATE_SENDING_BINDREQ 3
#define GRID_BTCPMUX_STATE_ACTIVE 4
#define GRID_BTCPMUX_STATE_STOPPING_USOCK 5
#define GRID_BTCPMUX_STATE_STOPPING_ATCPMUXES 6
#define GRID_BTCPMUX_STATE_LISTENING 7
#define GRID_BTCPMUX_STATE_WAITING 8
#define GRID_BTCPMUX_STATE_CLOSING 9
#define GRID_BTCPMUX_STATE_STOPPING_BACKOFF 10

#define GRID_BTCPMUX_SRC_USOCK 1
#define GRID_BTCPMUX_SRC_ATCPMUX 2
#define GRID_BTCPMUX_SRC_RECONNECT_TIMER 3

struct grid_btcpmux {

    /*  The state machine. */
    struct grid_fsm fsm;
    int state;

    /*  This object is a specific type of endpoint.
        Thus it is derived from epbase. */
    struct grid_epbase epbase;

    /*  The underlying listening TCPMUX socket. */
    struct grid_usock usock;

    /*  List of accepted connections. */
    struct grid_list atcpmuxes;

    /*  Used to wait before retrying to connect. */
    struct grid_backoff retry;

    /*  Service name. */
    const char *service;

    /*  Service name length, in network byte order. */
    uint16_t servicelen;

    /*  File descriptor of newly accepted connection. */
    int newfd;

    /*  Temporary buffer. */
    char code;
};

/*  grid_epbase virtual interface implementation. */
static void grid_btcpmux_stop (struct grid_epbase *self);
static void grid_btcpmux_destroy (struct grid_epbase *self);
const struct grid_epbase_vfptr grid_btcpmux_epbase_vfptr = {
    grid_btcpmux_stop,
    grid_btcpmux_destroy
};

/*  Private functions. */
static void grid_btcpmux_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_btcpmux_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_btcpmux_start_connecting (struct grid_btcpmux *self);

int grid_btcpmux_create (void *hint, struct grid_epbase **epbase)
{
    int rc;
    struct grid_btcpmux *self;
    const char *addr;
    const char *colon;
    const char *slash;
    const char *end;
    int reconnect_ivl;
    int reconnect_ivl_max;
    size_t sz;

    /*  Allocate the new endpoint object. */
    self = grid_alloc (sizeof (struct grid_btcpmux), "btcpmux");
    alloc_assert (self);

    /*  Initalise the epbase. */
    grid_epbase_init (&self->epbase, &grid_btcpmux_epbase_vfptr, hint);

    /*  Parse the connection string. For now, we can only bind to all
        interfaces. */
    addr = grid_epbase_getaddr (&self->epbase);
    colon = strchr (addr, ':');
    if (grid_slow (!colon || colon - addr != 1 || addr [0] != '*')) {
        grid_epbase_term (&self->epbase);
        return -EINVAL;
    }
    slash = strchr (colon + 1, '/');
    if (grid_slow (!slash)) {
        grid_epbase_term (&self->epbase);
        return -EINVAL;
    }
    end = addr + strlen (addr);

    /*  Parse the port. */
    rc = grid_port_resolve (colon + 1, slash - (colon + 1));
    if (grid_slow (rc < 0)) {
        grid_epbase_term (&self->epbase);
        return -EINVAL;
    }

    /*  Store the service name. */
    self->service = slash + 1;
    self->servicelen = htons (end - (slash + 1));

    /*  Initialise the structure. */
    grid_fsm_init_root (&self->fsm, grid_btcpmux_handler, grid_btcpmux_shutdown,
        grid_epbase_getctx (&self->epbase));
    self->state = GRID_BTCPMUX_STATE_IDLE;
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
    grid_backoff_init (&self->retry, GRID_BTCPMUX_SRC_RECONNECT_TIMER,
        reconnect_ivl, reconnect_ivl_max, &self->fsm);
    grid_usock_init (&self->usock, GRID_BTCPMUX_SRC_USOCK, &self->fsm);
    grid_list_init (&self->atcpmuxes);

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);

    /*  Return the base class as an out parameter. */
    *epbase = &self->epbase;

    return 0;
}

static void grid_btcpmux_stop (struct grid_epbase *self)
{
    struct grid_btcpmux *btcpmux;

    btcpmux = grid_cont (self, struct grid_btcpmux, epbase);

    grid_fsm_stop (&btcpmux->fsm);
}

static void grid_btcpmux_destroy (struct grid_epbase *self)
{
    struct grid_btcpmux *btcpmux;

    btcpmux = grid_cont (self, struct grid_btcpmux, epbase);

    grid_assert_state (btcpmux, GRID_BTCPMUX_STATE_IDLE);
    grid_list_term (&btcpmux->atcpmuxes);
    grid_usock_term (&btcpmux->usock);
    grid_backoff_term (&btcpmux->retry);
    grid_epbase_term (&btcpmux->epbase);
    grid_fsm_term (&btcpmux->fsm);

    grid_free (btcpmux);
}

static void grid_btcpmux_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    struct grid_btcpmux *btcpmux;
    struct grid_list_item *it;
    struct grid_atcpmux *atcpmux;

    btcpmux = grid_cont (self, struct grid_btcpmux, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        grid_backoff_stop (&btcpmux->retry);
        grid_usock_stop (&btcpmux->usock);
        btcpmux->state = GRID_BTCPMUX_STATE_STOPPING_USOCK;
    }
    if (grid_slow (btcpmux->state == GRID_BTCPMUX_STATE_STOPPING_USOCK)) {
       if (!grid_usock_isidle (&btcpmux->usock))
            return;
        for (it = grid_list_begin (&btcpmux->atcpmuxes);
              it != grid_list_end (&btcpmux->atcpmuxes);
              it = grid_list_next (&btcpmux->atcpmuxes, it)) {
            atcpmux = grid_cont (it, struct grid_atcpmux, item);
            grid_atcpmux_stop (atcpmux);
        }
        btcpmux->state = GRID_BTCPMUX_STATE_STOPPING_ATCPMUXES;
        goto atcpmuxes_stopping;
    }
    if (grid_slow (btcpmux->state == GRID_BTCPMUX_STATE_STOPPING_ATCPMUXES)) {
        grid_assert (src == GRID_BTCPMUX_SRC_ATCPMUX && type == GRID_ATCPMUX_STOPPED);
        atcpmux = (struct grid_atcpmux *) srcptr;
        grid_list_erase (&btcpmux->atcpmuxes, &atcpmux->item);
        grid_atcpmux_term (atcpmux);
        grid_free (atcpmux);

        /*  If there are no more atcpmux state machines, we can stop the whole
            btcpmux object. */
atcpmuxes_stopping:
        if (grid_list_empty (&btcpmux->atcpmuxes)) {
            btcpmux->state = GRID_BTCPMUX_STATE_IDLE;
            grid_fsm_stopped_noevent (&btcpmux->fsm);
            grid_epbase_stopped (&btcpmux->epbase);
            return;
        }

        return;
    }

    grid_fsm_bad_action(btcpmux->state, src, type);
}

static void grid_btcpmux_handler (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    struct grid_btcpmux *btcpmux;
    struct grid_atcpmux *atcpmux;
    struct grid_iovec iovecs [2];

    btcpmux = grid_cont (self, struct grid_btcpmux, fsm);

    switch (btcpmux->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case GRID_BTCPMUX_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_btcpmux_start_connecting (btcpmux);
                return;
            default:
                grid_fsm_bad_action (btcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (btcpmux->state, src, type);
        }

/******************************************************************************/
/*  CONNECTING state.                                                         */
/******************************************************************************/
    case GRID_BTCPMUX_STATE_CONNECTING:
        switch (src) {
        case GRID_BTCPMUX_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_CONNECTED:
                iovecs [0].iov_base = &btcpmux->servicelen;
                iovecs [0].iov_len = 2;
                iovecs [1].iov_base = (void*) btcpmux->service;
                iovecs [1].iov_len = ntohs (btcpmux->servicelen);
                grid_usock_send (&btcpmux->usock, iovecs, 2);
                btcpmux->state = GRID_BTCPMUX_STATE_SENDING_BINDREQ;
                return;
            case GRID_USOCK_ERROR:
                grid_usock_stop (&btcpmux->usock);
                btcpmux->state = GRID_BTCPMUX_STATE_STOPPING_USOCK;
                return;
            default:
                grid_fsm_bad_action (btcpmux->state, src, type);
            }
        default:
            grid_fsm_bad_source (btcpmux->state, src, type);
        }

/******************************************************************************/
/*  SENDING_BINDREQ state.                                                    */
/******************************************************************************/
    case GRID_BTCPMUX_STATE_SENDING_BINDREQ:
        switch (src) {
        case GRID_BTCPMUX_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SENT:
                grid_usock_recv (&btcpmux->usock, &btcpmux->code, 1,
                    &btcpmux->newfd);
                btcpmux->state = GRID_BTCPMUX_STATE_ACTIVE;
                return;
            case GRID_USOCK_ERROR:
                grid_usock_stop (&btcpmux->usock);
                btcpmux->state = GRID_BTCPMUX_STATE_STOPPING_USOCK;
                return;
            default:
                grid_fsm_bad_action (btcpmux->state, src, type);
            }
        default:
            grid_fsm_bad_source (btcpmux->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  The execution is yielded to the atcpmux state machine in this state.      */
/******************************************************************************/
    case GRID_BTCPMUX_STATE_ACTIVE:
        if (src == GRID_BTCPMUX_SRC_USOCK) {
            switch (type) {
            case GRID_USOCK_RECEIVED:
                if (btcpmux->code != 0 || btcpmux->newfd < 0) {
                    grid_usock_stop (&btcpmux->usock);
                    btcpmux->state = GRID_BTCPMUX_STATE_STOPPING_USOCK;
                    return;
                }

                /*  Allocate new atcpmux state machine. */
                atcpmux = grid_alloc (sizeof (struct grid_atcpmux), "atcpmux");
                alloc_assert (atcpmux);
                grid_atcpmux_init (atcpmux, GRID_BTCPMUX_SRC_ATCPMUX,
                   &btcpmux->epbase, &btcpmux->fsm);
                grid_atcpmux_start (atcpmux, btcpmux->newfd);

                grid_list_insert (&btcpmux->atcpmuxes, &atcpmux->item,
                    grid_list_end (&btcpmux->atcpmuxes));

                /*  Start accepting new connection straight away. */
                grid_usock_recv (&btcpmux->usock, &btcpmux->code, 1,
                    &btcpmux->newfd);
                return;
            case GRID_USOCK_ERROR:
                grid_usock_stop (&btcpmux->usock);
                btcpmux->state = GRID_BTCPMUX_STATE_STOPPING_USOCK;
                return;
            default:
                grid_fsm_bad_action (btcpmux->state, src, type);
            }
        }

        /*  For all remaining events we'll assume they are coming from one
            of remaining child atcpmux objects. */
        grid_assert (src == GRID_BTCPMUX_SRC_ATCPMUX);
        atcpmux = (struct grid_atcpmux*) srcptr;
        switch (type) {
        case GRID_ATCPMUX_ERROR:
            grid_atcpmux_stop (atcpmux);
            return;
        case GRID_ATCPMUX_STOPPED:
            grid_list_erase (&btcpmux->atcpmuxes, &atcpmux->item);
            grid_atcpmux_term (atcpmux);
            grid_free (atcpmux);
            return;
        default:
            grid_fsm_bad_action (btcpmux->state, src, type);
        }

/******************************************************************************/
/*  CLOSING_USOCK state.                                                     */
/*  usock object was asked to stop but it haven't stopped yet.                */
/******************************************************************************/
    case GRID_BTCPMUX_STATE_CLOSING:
        switch (src) {

        case GRID_BTCPMUX_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_USOCK_STOPPED:
                grid_backoff_start (&btcpmux->retry);
                btcpmux->state = GRID_BTCPMUX_STATE_WAITING;
                return;
            default:
                grid_fsm_bad_action (btcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (btcpmux->state, src, type);
        }

/******************************************************************************/
/*  WAITING state.                                                            */
/*  Waiting before re-bind is attempted. This way we won't overload           */
/*  the system by continuous re-bind attemps.                                 */
/******************************************************************************/
    case GRID_BTCPMUX_STATE_WAITING:
        switch (src) {

        case GRID_BTCPMUX_SRC_RECONNECT_TIMER:
            switch (type) {
            case GRID_BACKOFF_TIMEOUT:
                grid_backoff_stop (&btcpmux->retry);
                btcpmux->state = GRID_BTCPMUX_STATE_STOPPING_BACKOFF;
                return;
            default:
                grid_fsm_bad_action (btcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (btcpmux->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_BACKOFF state.                                                   */
/*  backoff object was asked to stop, but it haven't stopped yet.             */
/******************************************************************************/
    case GRID_BTCPMUX_STATE_STOPPING_BACKOFF:
        switch (src) {

        case GRID_BTCPMUX_SRC_RECONNECT_TIMER:
            switch (type) {
            case GRID_BACKOFF_STOPPED:
                grid_btcpmux_start_connecting (btcpmux);
                return;
            default:
                grid_fsm_bad_action (btcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (btcpmux->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (btcpmux->state, src, type);
    }
}

/******************************************************************************/
/*  State machine actions.                                                    */
/******************************************************************************/

static void grid_btcpmux_start_connecting (struct grid_btcpmux *self)
{
    int rc;
    struct sockaddr_storage ss;
    struct sockaddr_un *un;
    const char *addr;
    const char *colon;
    const char *slash;
    int port;

    /*  Try to start the underlying socket. */
    rc = grid_usock_start (&self->usock, AF_UNIX, SOCK_STREAM, 0);
    if (grid_slow (rc < 0)) {
        grid_backoff_start (&self->retry);
        self->state = GRID_BTCPMUX_STATE_WAITING;
        return;
    }

    /*  Create the IPC address from the address string. */
    addr = grid_epbase_getaddr (&self->epbase);
    colon = strchr (addr, ':');
    slash = strchr (colon + 1, '/');

    port = grid_port_resolve (colon + 1, slash - (colon + 1));
    memset (&ss, 0, sizeof (ss));
    un = (struct sockaddr_un*) &ss;
    ss.ss_family = AF_UNIX;
    sprintf (un->sun_path, "/tmp/tcpmux-%d.ipc", (int) port);

    /*  Start connecting. */
    grid_usock_connect (&self->usock, (struct sockaddr*) &ss,
        sizeof (struct sockaddr_un));
    self->state  = GRID_BTCPMUX_STATE_CONNECTING;
}


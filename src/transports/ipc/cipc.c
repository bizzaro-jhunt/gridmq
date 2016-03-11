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

#include "cipc.h"
#include "sipc.h"

#include "../../aio/fsm.h"
#include "../../aio/usock.h"

#include "../utils/backoff.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/fast.h"
#include "../../utils/attr.h"

#include <string.h>
#if defined GRID_HAVE_WINDOWS
#include "../../utils/win.h"
#else
#include <unistd.h>
#include <sys/un.h>
#endif

#define GRID_CIPC_STATE_IDLE 1
#define GRID_CIPC_STATE_CONNECTING 2
#define GRID_CIPC_STATE_ACTIVE 3
#define GRID_CIPC_STATE_STOPPING_SIPC 4
#define GRID_CIPC_STATE_STOPPING_USOCK 5
#define GRID_CIPC_STATE_WAITING 6
#define GRID_CIPC_STATE_STOPPING_BACKOFF 7
#define GRID_CIPC_STATE_STOPPING_SIPC_FINAL 8
#define GRID_CIPC_STATE_STOPPING 9

#define GRID_CIPC_SRC_USOCK 1
#define GRID_CIPC_SRC_RECONNECT_TIMER 2
#define GRID_CIPC_SRC_SIPC 3

struct grid_cipc {

    /*  The state machine. */
    struct grid_fsm fsm;
    int state;

    /*  This object is a specific type of endpoint.
        Thus it is derived from epbase. */
    struct grid_epbase epbase;

    /*  The underlying IPC socket. */
    struct grid_usock usock;

    /*  Used to wait before retrying to connect. */
    struct grid_backoff retry;

    /*  State machine that handles the active part of the connection
        lifetime. */
    struct grid_sipc sipc;
};

/*  grid_epbase virtual interface implementation. */
static void grid_cipc_stop (struct grid_epbase *self);
static void grid_cipc_destroy (struct grid_epbase *self);
const struct grid_epbase_vfptr grid_cipc_epbase_vfptr = {
    grid_cipc_stop,
    grid_cipc_destroy
};

/*  Private functions. */
static void grid_cipc_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_cipc_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_cipc_start_connecting (struct grid_cipc *self);

int grid_cipc_create (void *hint, struct grid_epbase **epbase)
{
    struct grid_cipc *self;
    int reconnect_ivl;
    int reconnect_ivl_max;
    size_t sz;

    /*  Allocate the new endpoint object. */
    self = grid_alloc (sizeof (struct grid_cipc), "cipc");
    alloc_assert (self);

    /*  Initialise the structure. */
    grid_epbase_init (&self->epbase, &grid_cipc_epbase_vfptr, hint);
    grid_fsm_init_root (&self->fsm, grid_cipc_handler, grid_cipc_shutdown,
        grid_epbase_getctx (&self->epbase));
    self->state = GRID_CIPC_STATE_IDLE;
    grid_usock_init (&self->usock, GRID_CIPC_SRC_USOCK, &self->fsm);
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
    grid_backoff_init (&self->retry, GRID_CIPC_SRC_RECONNECT_TIMER,
        reconnect_ivl, reconnect_ivl_max, &self->fsm);
    grid_sipc_init (&self->sipc, GRID_CIPC_SRC_SIPC, &self->epbase, &self->fsm);

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);

    /*  Return the base class as an out parameter. */
    *epbase = &self->epbase;

    return 0;
}

static void grid_cipc_stop (struct grid_epbase *self)
{
    struct grid_cipc *cipc;

    cipc = grid_cont (self, struct grid_cipc, epbase);

    grid_fsm_stop (&cipc->fsm);
}

static void grid_cipc_destroy (struct grid_epbase *self)
{
    struct grid_cipc *cipc;

    cipc = grid_cont (self, struct grid_cipc, epbase);

    grid_sipc_term (&cipc->sipc);
    grid_backoff_term (&cipc->retry);
    grid_usock_term (&cipc->usock);
    grid_fsm_term (&cipc->fsm);
    grid_epbase_term (&cipc->epbase);

    grid_free (cipc);
}

static void grid_cipc_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_cipc *cipc;

    cipc = grid_cont (self, struct grid_cipc, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        if (!grid_sipc_isidle (&cipc->sipc)) {
            grid_epbase_stat_increment (&cipc->epbase,
                GRID_STAT_DROPPED_CONNECTIONS, 1);
            grid_sipc_stop (&cipc->sipc);
        }
        cipc->state = GRID_CIPC_STATE_STOPPING_SIPC_FINAL;
    }
    if (grid_slow (cipc->state == GRID_CIPC_STATE_STOPPING_SIPC_FINAL)) {
        if (!grid_sipc_isidle (&cipc->sipc))
            return;
        grid_backoff_stop (&cipc->retry);
        grid_usock_stop (&cipc->usock);
        cipc->state = GRID_CIPC_STATE_STOPPING;
    }
    if (grid_slow (cipc->state == GRID_CIPC_STATE_STOPPING)) {
        if (!grid_backoff_isidle (&cipc->retry) ||
              !grid_usock_isidle (&cipc->usock))
            return;
        cipc->state = GRID_CIPC_STATE_IDLE;
        grid_fsm_stopped_noevent (&cipc->fsm);
        grid_epbase_stopped (&cipc->epbase);
        return;
    }

    grid_fsm_bad_state(cipc->state, src, type);
}

static void grid_cipc_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_cipc *cipc;

    cipc = grid_cont (self, struct grid_cipc, fsm);

    switch (cipc->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  The state machine wasn't yet started.                                     */
/******************************************************************************/
    case GRID_CIPC_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_cipc_start_connecting (cipc);
                return;
            default:
                grid_fsm_bad_action (cipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (cipc->state, src, type);
        }

/******************************************************************************/
/*  CONNECTING state.                                                         */
/*  Non-blocking connect is under way.                                        */
/******************************************************************************/
    case GRID_CIPC_STATE_CONNECTING:
        switch (src) {

        case GRID_CIPC_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_CONNECTED:
                grid_sipc_start (&cipc->sipc, &cipc->usock);
                cipc->state = GRID_CIPC_STATE_ACTIVE;
                grid_epbase_stat_increment (&cipc->epbase,
                    GRID_STAT_INPROGRESS_CONNECTIONS, -1);
                grid_epbase_stat_increment (&cipc->epbase,
                    GRID_STAT_ESTABLISHED_CONNECTIONS, 1);
                grid_epbase_clear_error (&cipc->epbase);
                return;
            case GRID_USOCK_ERROR:
                grid_epbase_set_error (&cipc->epbase,
                    grid_usock_geterrno (&cipc->usock));
                grid_usock_stop (&cipc->usock);
                cipc->state = GRID_CIPC_STATE_STOPPING_USOCK;
                grid_epbase_stat_increment (&cipc->epbase,
                    GRID_STAT_INPROGRESS_CONNECTIONS, -1);
                grid_epbase_stat_increment (&cipc->epbase,
                    GRID_STAT_CONNECT_ERRORS, 1);
                return;
            default:
                grid_fsm_bad_action (cipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (cipc->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  Connection is established and handled by the sipc state machine.          */
/******************************************************************************/
    case GRID_CIPC_STATE_ACTIVE:
        switch (src) {

        case GRID_CIPC_SRC_SIPC:
            switch (type) {
            case GRID_SIPC_ERROR:
                grid_sipc_stop (&cipc->sipc);
                cipc->state = GRID_CIPC_STATE_STOPPING_SIPC;
                grid_epbase_stat_increment (&cipc->epbase,
                    GRID_STAT_BROKEN_CONNECTIONS, 1);
                return;
            default:
               grid_fsm_bad_action (cipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (cipc->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_SIPC state.                                                      */
/*  sipc object was asked to stop but it haven't stopped yet.                 */
/******************************************************************************/
    case GRID_CIPC_STATE_STOPPING_SIPC:
        switch (src) {

        case GRID_CIPC_SRC_SIPC:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_SIPC_STOPPED:
                grid_usock_stop (&cipc->usock);
                cipc->state = GRID_CIPC_STATE_STOPPING_USOCK;
                return;
            default:
                grid_fsm_bad_action (cipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (cipc->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_USOCK state.                                                     */
/*  usock object was asked to stop but it haven't stopped yet.                */
/******************************************************************************/
    case GRID_CIPC_STATE_STOPPING_USOCK:
        switch (src) {

        case GRID_CIPC_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_USOCK_STOPPED:
                grid_backoff_start (&cipc->retry);
                cipc->state = GRID_CIPC_STATE_WAITING;
                return;
            default:
                grid_fsm_bad_action (cipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (cipc->state, src, type);
        }

/******************************************************************************/
/*  WAITING state.                                                            */
/*  Waiting before re-connection is attempted. This way we won't overload     */
/*  the system by continuous re-connection attemps.                           */
/******************************************************************************/
    case GRID_CIPC_STATE_WAITING:
        switch (src) {

        case GRID_CIPC_SRC_RECONNECT_TIMER:
            switch (type) {
            case GRID_BACKOFF_TIMEOUT:
                grid_backoff_stop (&cipc->retry);
                cipc->state = GRID_CIPC_STATE_STOPPING_BACKOFF;
                return;
            default:
                grid_fsm_bad_action (cipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (cipc->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_BACKOFF state.                                                   */
/*  backoff object was asked to stop, but it haven't stopped yet.             */
/******************************************************************************/
    case GRID_CIPC_STATE_STOPPING_BACKOFF:
        switch (src) {

        case GRID_CIPC_SRC_RECONNECT_TIMER:
            switch (type) {
            case GRID_BACKOFF_STOPPED:
                grid_cipc_start_connecting (cipc);
                return;
            default:
                grid_fsm_bad_action (cipc->state, src, type);
            }

        default:
            grid_fsm_bad_source (cipc->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (cipc->state, src, type);
    }
}

/******************************************************************************/
/*  State machine actions.                                                    */
/******************************************************************************/

static void grid_cipc_start_connecting (struct grid_cipc *self)
{
    int rc;
    struct sockaddr_storage ss;
    struct sockaddr_un *un;
    const char *addr;
    int val;
    size_t sz;

    /*  Try to start the underlying socket. */
    rc = grid_usock_start (&self->usock, AF_UNIX, SOCK_STREAM, 0);
    if (grid_slow (rc < 0)) {
        grid_backoff_start (&self->retry);
        self->state = GRID_CIPC_STATE_WAITING;
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

    /*  Create the IPC address from the address string. */
    addr = grid_epbase_getaddr (&self->epbase);
    memset (&ss, 0, sizeof (ss));
    un = (struct sockaddr_un*) &ss;
    grid_assert (strlen (addr) < sizeof (un->sun_path));
    ss.ss_family = AF_UNIX;
    strncpy (un->sun_path, addr, sizeof (un->sun_path));

#if defined GRID_HAVE_WINDOWS
    /* Get/Set security attribute pointer*/
    grid_epbase_getopt (&self->epbase, GRID_IPC, GRID_IPC_SEC_ATTR, &self->usock.sec_attr, &sz);

    grid_epbase_getopt (&self->epbase, GRID_IPC, GRID_IPC_OUTBUFSZ, &self->usock.outbuffersz, &sz);
    grid_epbase_getopt (&self->epbase, GRID_IPC, GRID_IPC_INBUFSZ, &self->usock.inbuffersz, &sz);
#endif

    /*  Start connecting. */
    grid_usock_connect (&self->usock, (struct sockaddr*) &ss,
        sizeof (struct sockaddr_un));
    self->state  = GRID_CIPC_STATE_CONNECTING;

    grid_epbase_stat_increment (&self->epbase,
        GRID_STAT_INPROGRESS_CONNECTIONS, 1);
}


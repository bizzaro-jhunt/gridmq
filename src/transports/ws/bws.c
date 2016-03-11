/*
    Copyright (c) 2012-2013 250bpm s.r.o.  All rights reserved.
    Copyright (c) 2014 Wirebird Labs LLC.  All rights reserved.

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

#include "bws.h"
#include "aws.h"

#include "../utils/port.h"
#include "../utils/iface.h"

#include "../../aio/fsm.h"
#include "../../aio/usock.h"

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
#define GRID_BWS_BACKLOG 100

#define GRID_BWS_STATE_IDLE 1
#define GRID_BWS_STATE_ACTIVE 2
#define GRID_BWS_STATE_STOPPING_AWS 3
#define GRID_BWS_STATE_STOPPING_USOCK 4
#define GRID_BWS_STATE_STOPPING_AWSS 5

#define GRID_BWS_SRC_USOCK 1
#define GRID_BWS_SRC_AWS 2

struct grid_bws {

    /*  The state machine. */
    struct grid_fsm fsm;
    int state;

    /*  This object is a specific type of endpoint.
        Thus it is derived from epbase. */
    struct grid_epbase epbase;

    /*  The underlying listening WS socket. */
    struct grid_usock usock;

    /*  The connection being accepted at the moment. */
    struct grid_aws *aws;

    /*  List of accepted connections. */
    struct grid_list awss;
};

/*  grid_epbase virtual interface implementation. */
static void grid_bws_stop (struct grid_epbase *self);
static void grid_bws_destroy (struct grid_epbase *self);
const struct grid_epbase_vfptr grid_bws_epbase_vfptr = {
    grid_bws_stop,
    grid_bws_destroy
};

/*  Private functions. */
static void grid_bws_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_bws_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_bws_start_listening (struct grid_bws *self);
static void grid_bws_start_accepting (struct grid_bws *self);

int grid_bws_create (void *hint, struct grid_epbase **epbase)
{
    int rc;
    struct grid_bws *self;
    const char *addr;
    const char *end;
    const char *pos;
    struct sockaddr_storage ss;
    size_t sslen;
    int ipv4only;
    size_t ipv4onlylen;

    /*  Allocate the new endpoint object. */
    self = grid_alloc (sizeof (struct grid_bws), "bws");
    alloc_assert (self);

    /*  Initalise the epbase. */
    grid_epbase_init (&self->epbase, &grid_bws_epbase_vfptr, hint);
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
    grid_fsm_init_root (&self->fsm, grid_bws_handler, grid_bws_shutdown,
        grid_epbase_getctx (&self->epbase));
    self->state = GRID_BWS_STATE_IDLE;
    grid_usock_init (&self->usock, GRID_BWS_SRC_USOCK, &self->fsm);
    self->aws = NULL;
    grid_list_init (&self->awss);

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);

    /*  Return the base class as an out parameter. */
    *epbase = &self->epbase;

    return 0;
}

static void grid_bws_stop (struct grid_epbase *self)
{
    struct grid_bws *bws;

    bws = grid_cont (self, struct grid_bws, epbase);

    grid_fsm_stop (&bws->fsm);
}

static void grid_bws_destroy (struct grid_epbase *self)
{
    struct grid_bws *bws;

    bws = grid_cont (self, struct grid_bws, epbase);

    grid_assert_state (bws, GRID_BWS_STATE_IDLE);
    grid_list_term (&bws->awss);
    grid_assert (bws->aws == NULL);
    grid_usock_term (&bws->usock);
    grid_epbase_term (&bws->epbase);
    grid_fsm_term (&bws->fsm);

    grid_free (bws);
}

static void grid_bws_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    struct grid_bws *bws;
    struct grid_list_item *it;
    struct grid_aws *aws;

    bws = grid_cont (self, struct grid_bws, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        grid_aws_stop (bws->aws);
        bws->state = GRID_BWS_STATE_STOPPING_AWS;
    }
    if (grid_slow (bws->state == GRID_BWS_STATE_STOPPING_AWS)) {
        if (!grid_aws_isidle (bws->aws))
            return;
        grid_aws_term (bws->aws);
        grid_free (bws->aws);
        bws->aws = NULL;
        grid_usock_stop (&bws->usock);
        bws->state = GRID_BWS_STATE_STOPPING_USOCK;
    }
    if (grid_slow (bws->state == GRID_BWS_STATE_STOPPING_USOCK)) {
       if (!grid_usock_isidle (&bws->usock))
            return;
        for (it = grid_list_begin (&bws->awss);
              it != grid_list_end (&bws->awss);
              it = grid_list_next (&bws->awss, it)) {
            aws = grid_cont (it, struct grid_aws, item);
            grid_aws_stop (aws);
        }
        bws->state = GRID_BWS_STATE_STOPPING_AWSS;
        goto awss_stopping;
    }
    if (grid_slow (bws->state == GRID_BWS_STATE_STOPPING_AWSS)) {
        grid_assert (src == GRID_BWS_SRC_AWS && type == GRID_AWS_STOPPED);
        aws = (struct grid_aws *) srcptr;
        grid_list_erase (&bws->awss, &aws->item);
        grid_aws_term (aws);
        grid_free (aws);

        /*  If there are no more aws state machines, we can stop the whole
            bws object. */
awss_stopping:
        if (grid_list_empty (&bws->awss)) {
            bws->state = GRID_BWS_STATE_IDLE;
            grid_fsm_stopped_noevent (&bws->fsm);
            grid_epbase_stopped (&bws->epbase);
            return;
        }

        return;
    }

    grid_fsm_bad_action (bws->state, src, type);
}

static void grid_bws_handler (struct grid_fsm *self, int src, int type,
    void *srcptr)
{
    struct grid_bws *bws;
    struct grid_aws *aws;

    bws = grid_cont (self, struct grid_bws, fsm);

    switch (bws->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case GRID_BWS_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_bws_start_listening (bws);
                grid_bws_start_accepting (bws);
                bws->state = GRID_BWS_STATE_ACTIVE;
                return;
            default:
                grid_fsm_bad_action (bws->state, src, type);
            }

        default:
            grid_fsm_bad_source (bws->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/*  The execution is yielded to the aws state machine in this state.          */
/******************************************************************************/
    case GRID_BWS_STATE_ACTIVE:
        if (srcptr == bws->aws) {
            switch (type) {
            case GRID_AWS_ACCEPTED:

                /*  Move the newly created connection to the list of existing
                    connections. */
                grid_list_insert (&bws->awss, &bws->aws->item,
                    grid_list_end (&bws->awss));
                bws->aws = NULL;

                /*  Start waiting for a new incoming connection. */
                grid_bws_start_accepting (bws);

                return;

            default:
                grid_fsm_bad_action (bws->state, src, type);
            }
        }

        /*  For all remaining events we'll assume they are coming from one
            of remaining child aws objects. */
        grid_assert (src == GRID_BWS_SRC_AWS);
        aws = (struct grid_aws*) srcptr;
        switch (type) {
        case GRID_AWS_ERROR:
            grid_aws_stop (aws);
            return;
        case GRID_AWS_STOPPED:
            grid_list_erase (&bws->awss, &aws->item);
            grid_aws_term (aws);
            grid_free (aws);
            return;
        default:
            grid_fsm_bad_action (bws->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (bws->state, src, type);
    }
}

/******************************************************************************/
/*  State machine actions.                                                    */
/******************************************************************************/

static void grid_bws_start_listening (struct grid_bws *self)
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
    /*  TODO: EMFILE error can happen here. We can wait a bit and re-try. */
    errnum_assert (rc == 0, -rc);
    rc = grid_usock_bind (&self->usock, (struct sockaddr*) &ss, (size_t) sslen);
    errnum_assert (rc == 0, -rc);
    rc = grid_usock_listen (&self->usock, GRID_BWS_BACKLOG);
    errnum_assert (rc == 0, -rc);
}

static void grid_bws_start_accepting (struct grid_bws *self)
{
    grid_assert (self->aws == NULL);

    /*  Allocate new aws state machine. */
    self->aws = grid_alloc (sizeof (struct grid_aws), "aws");
    alloc_assert (self->aws);
    grid_aws_init (self->aws, GRID_BWS_SRC_AWS, &self->epbase, &self->fsm);

    /*  Start waiting for a new incoming connection. */
    grid_aws_start (self->aws, &self->usock);
}


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

#include "literal.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/attr.h"

#include <string.h>

#ifndef GRID_HAVE_WINDOWS
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#endif

#define GRID_DNS_STATE_IDLE 1
#define GRID_DNS_STATE_DONE 2

/*  Private functions. */
static void grid_dns_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_dns_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);

void grid_dns_init (struct grid_dns *self, int src, struct grid_fsm *owner)
{
    grid_fsm_init (&self->fsm, grid_dns_handler, grid_dns_shutdown,
        src, self, owner);
    self->state = GRID_DNS_STATE_IDLE;
    grid_fsm_event_init (&self->done);
}

void grid_dns_term (struct grid_dns *self)
{
    grid_assert_state (self, GRID_DNS_STATE_IDLE);

    grid_fsm_event_term (&self->done);
    grid_fsm_term (&self->fsm);
}

int grid_dns_isidle (struct grid_dns *self)
{
    return grid_fsm_isidle (&self->fsm);
}

void grid_dns_start (struct grid_dns *self, const char *addr, size_t addrlen,
    int ipv4only, struct grid_dns_result *result)
{
    int rc;
    struct addrinfo query;
    struct addrinfo *reply;
    char hostname [GRID_SOCKADDR_MAX];

    grid_assert_state (self, GRID_DNS_STATE_IDLE);

    self->result = result;

    /*  Try to resolve the supplied string as a literal address. In this case,
        there's no DNS lookup involved. */
    rc = grid_literal_resolve (addr, addrlen, ipv4only, &self->result->addr,
        &self->result->addrlen);
    if (rc == 0) {
        self->result->error = 0;
        grid_fsm_start (&self->fsm);
        return;
    }
    errnum_assert (rc == -EINVAL, -rc);

    /*  The name is not a literal. Let's do an actual DNS lookup. */
    memset (&query, 0, sizeof (query));
    if (ipv4only)
        query.ai_family = AF_INET;
    else {
        query.ai_family = AF_INET6;
#ifdef AI_V4MAPPED
        query.ai_flags = AI_V4MAPPED;
#endif
    }
    grid_assert (sizeof (hostname) > addrlen);
    query.ai_socktype = SOCK_STREAM;
    memcpy (hostname, addr, addrlen);
    hostname [addrlen] = 0;

    /*  Perform the DNS lookup itself. */
    self->result->error = getaddrinfo (hostname, NULL, &query, &reply);
    if (self->result->error) {
        grid_fsm_start (&self->fsm);
        return;
    }

    /*  Take just the first address and store it.  (RFC recommends that we
        iterate through addresses until one works, but that doesn't match
        our state model.  This is the best we can do.) */
    self->result->error = 0;
    memcpy (&self->result->addr, reply->ai_addr, reply->ai_addrlen);
    self->result->addrlen = reply->ai_addrlen;
    freeaddrinfo (reply);

    grid_fsm_start (&self->fsm);
}

void grid_dns_stop (struct grid_dns *self)
{
    grid_fsm_stop (&self->fsm);
}

static void grid_dns_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_dns *dns;

    dns = grid_cont (self, struct grid_dns, fsm);
    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        grid_fsm_stopped (&dns->fsm, GRID_DNS_STOPPED);
        dns->state = GRID_DNS_STATE_IDLE;
        return;
    }

    grid_fsm_bad_state(dns->state, src, type);
}

static void grid_dns_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_dns *dns;

    dns = grid_cont (self, struct grid_dns, fsm);

    switch (dns->state) {
/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case GRID_DNS_STATE_IDLE:
        switch (src) {
        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_fsm_raise (&dns->fsm, &dns->done, GRID_DNS_DONE);
                dns->state = GRID_DNS_STATE_DONE;
                return;
            default:
                grid_fsm_bad_action (dns->state, src, type);
            }
        default:
            grid_fsm_bad_source (dns->state, src, type);
        }

/******************************************************************************/
/*  DONE state.                                                               */
/******************************************************************************/
    case GRID_DNS_STATE_DONE:
        grid_fsm_bad_source (dns->state, src, type);

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (dns->state, src, type);
    }
}


/*
    Copyright (c) 2013 250bpm s.r.o.  All rights reserved.
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

#include "ws_handshake.h"
#include "sha1.h"

#include "../../aio/timer.h"

#include "../../core/sock.h"

#include "../utils/base64.h"

#include "../../utils/alloc.h"
#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/wire.h"
#include "../../utils/attr.h"
#include "../../utils/random.h"

#include <stddef.h>
#include <string.h>
#include <ctype.h>

/*****************************************************************************/
/***  BEGIN undesirable dependency *******************************************/
/*****************************************************************************/
/*  TODO: A transport should be SP agnostic; alas, these includes are        */
/*        required for the map. Ideally, this map would live in another      */
/*        abstraction layer; perhaps a "registry" of Scalability Protocols?  */
/*****************************************************************************/
#include "../../pair.h"
#include "../../reqrep.h"
#include "../../pubsub.h"
#include "../../survey.h"
#include "../../pipeline.h"
#include "../../bus.h"

static const struct grid_ws_sp_map GRID_WS_HANDSHAKE_SP_MAP[] = {
    { GRID_PAIR,       GRID_PAIR,       "pair.sp.gridmq.net" },
    { GRID_REQ,        GRID_REP,        "req.sp.gridmq.net" },
    { GRID_REP,        GRID_REQ,        "rep.sp.gridmq.net" },
    { GRID_PUB,        GRID_SUB,        "pub.sp.gridmq.net" },
    { GRID_SUB,        GRID_PUB,        "sub.sp.gridmq.net" },
    { GRID_SURVEYOR,   GRID_RESPONDENT, "surveyor.sp.gridmq.net" },
    { GRID_RESPONDENT, GRID_SURVEYOR,   "respondent.sp.gridmq.net" },
    { GRID_PUSH,       GRID_PULL,       "push.sp.gridmq.net" },
    { GRID_PULL,       GRID_PUSH,       "pull.sp.gridmq.net" },
    { GRID_BUS,        GRID_BUS,        "bus.sp.gridmq.net" }
};

const size_t GRID_WS_HANDSHAKE_SP_MAP_LEN = sizeof (GRID_WS_HANDSHAKE_SP_MAP) /
    sizeof (GRID_WS_HANDSHAKE_SP_MAP [0]);
/*****************************************************************************/
/***  END undesirable dependency *********************************************/
/*****************************************************************************/

/*  State machine finite states. */
#define GRID_WS_HANDSHAKE_STATE_IDLE 1
#define GRID_WS_HANDSHAKE_STATE_SERVER_RECV 2
#define GRID_WS_HANDSHAKE_STATE_SERVER_REPLY 3
#define GRID_WS_HANDSHAKE_STATE_CLIENT_SEND 4
#define GRID_WS_HANDSHAKE_STATE_CLIENT_RECV 5
#define GRID_WS_HANDSHAKE_STATE_HANDSHAKE_SENT 6
#define GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_ERROR 7
#define GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_DONE 8
#define GRID_WS_HANDSHAKE_STATE_DONE 9
#define GRID_WS_HANDSHAKE_STATE_STOPPING 10

/*  Subordinate srcptr objects. */
#define GRID_WS_HANDSHAKE_SRC_USOCK 1
#define GRID_WS_HANDSHAKE_SRC_TIMER 2

/*  Time allowed to complete handshake. */
#define GRID_WS_HANDSHAKE_TIMEOUT 5000

/*  Possible return codes internal to the parsing operations. */
#define GRID_WS_HANDSHAKE_NOMATCH 0
#define GRID_WS_HANDSHAKE_MATCH 1

/*  Possible return codes from parsing opening handshake from peer. */
#define GRID_WS_HANDSHAKE_VALID 0
#define GRID_WS_HANDSHAKE_RECV_MORE 1
#define GRID_WS_HANDSHAKE_INVALID -1

/*  Possible handshake responses to send to client when acting as server. */
#define GRID_WS_HANDSHAKE_RESPONSE_NULL -1
#define GRID_WS_HANDSHAKE_RESPONSE_OK 0
#define GRID_WS_HANDSHAKE_RESPONSE_TOO_BIG 1
#define GRID_WS_HANDSHAKE_RESPONSE_UNUSED2 2
#define GRID_WS_HANDSHAKE_RESPONSE_WSPROTO 3
#define GRID_WS_HANDSHAKE_RESPONSE_WSVERSION 4
#define GRID_WS_HANDSHAKE_RESPONSE_GRIDPROTO 5
#define GRID_WS_HANDSHAKE_RESPONSE_NOTPEER 6
#define GRID_WS_HANDSHAKE_RESPONSE_UNKNOWNTYPE 7

/*  Private functions. */
static void grid_ws_handshake_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_ws_handshake_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_ws_handshake_leave (struct grid_ws_handshake *self, int rc);

/*  WebSocket protocol support functions. */
static int grid_ws_handshake_parse_client_opening (struct grid_ws_handshake *self);
static void grid_ws_handshake_server_reply (struct grid_ws_handshake *self);
static void grid_ws_handshake_client_request (struct grid_ws_handshake *self);
static int grid_ws_handshake_parse_server_response (struct grid_ws_handshake *self);
static int grid_ws_handshake_hash_key (const char *key, size_t key_len,
    char *hashed, size_t hashed_len);

/*  String parsing support functions. */

/*  Scans for reference token against subject string, optionally ignoring
    case sensitivity and/or leading spaces in subject. On match, advances
    the subject pointer to the next non-ignored character past match. Both
    strings must be NULL terminated to avoid undefined behavior. Returns
    GRID_WS_HANDSHAKE_MATCH on match; else, GRID_WS_HANDSHAKE_NOMATCH. */
static int grid_ws_match_token (const char* token, const char **subj,
    int case_insensitive, int ignore_leading_sp);

/*  Scans subject string for termination sequence, optionally ignoring
    leading and/or trailing spaces in subject. On match, advances
    the subject pointer to the next character past match. Both
    strings must be NULL terminated to avoid undefined behavior. If the
    match succeeds, values are stored into *addr and *len. */
static int grid_ws_match_value (const char* termseq, const char **subj,
    int ignore_leading_sp, int ignore_trailing_sp, const char **addr,
    size_t* const len);

/*  Compares subject octet stream to expected value, optionally ignoring
    case sensitivity. Returns non-zero on success, zero on failure. */
static int grid_ws_validate_value (const char* expected, const char *subj,
    size_t subj_len, int case_insensitive);

void grid_ws_handshake_init (struct grid_ws_handshake *self, int src,
    struct grid_fsm *owner)
{
    grid_fsm_init (&self->fsm, grid_ws_handshake_handler, grid_ws_handshake_shutdown,
        src, self, owner);
    self->state = GRID_WS_HANDSHAKE_STATE_IDLE;
    grid_timer_init (&self->timer, GRID_WS_HANDSHAKE_SRC_TIMER, &self->fsm);
    grid_fsm_event_init (&self->done);
    self->timeout = GRID_WS_HANDSHAKE_TIMEOUT;
    self->usock = NULL;
    self->usock_owner.src = -1;
    self->usock_owner.fsm = NULL;
    self->pipebase = NULL;
}

void grid_ws_handshake_term (struct grid_ws_handshake *self)
{
    grid_assert_state (self, GRID_WS_HANDSHAKE_STATE_IDLE);

    grid_fsm_event_term (&self->done);
    grid_timer_term (&self->timer);
    grid_fsm_term (&self->fsm);
}

int grid_ws_handshake_isidle (struct grid_ws_handshake *self)
{
    return grid_fsm_isidle (&self->fsm);
}

void grid_ws_handshake_start (struct grid_ws_handshake *self,
    struct grid_usock *usock, struct grid_pipebase *pipebase,
    int mode, const char *resource, const char *host)
{
    /*  It's expected this resource has been allocated during intial connect. */
    if (mode == GRID_WS_CLIENT)
        grid_assert (strlen (resource) >= 1);

    /*  Take ownership of the underlying socket. */
    grid_assert (self->usock == NULL && self->usock_owner.fsm == NULL);
    self->usock_owner.src = GRID_WS_HANDSHAKE_SRC_USOCK;
    self->usock_owner.fsm = &self->fsm;
    grid_usock_swap_owner (usock, &self->usock_owner);
    self->usock = usock;
    self->pipebase = pipebase;
    self->mode = mode;
    self->resource = resource;
    self->remote_host = host;

    memset (self->opening_hs, 0, sizeof (self->opening_hs));
    memset (self->response, 0, sizeof (self->response));

    self->recv_pos = 0;
    self->retries = 0;

    /*  Calculate the absolute minimum length possible for a valid opening
        handshake. This is an optimization since we must poll for the
        remainder of the opening handshake in small byte chunks. */
    switch (self->mode) {
    case GRID_WS_SERVER:
        self->recv_len = strlen (
            "GET x HTTP/1.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Host: x\r\n"
            "Origin: x\r\n"
            "Sec-WebSocket-Key: xxxxxxxxxxxxxxxxxxxxxxxx\r\n"
            "Sec-WebSocket-Version: xx\r\n\r\n");
        break;
    case GRID_WS_CLIENT:
        /*  Shortest conceiveable response from server is a terse status. */
        self->recv_len = strlen ("HTTP/1.1 xxx\r\n\r\n");
        break;
    default:
        /*  Developer error; unexpected mode. */
        grid_assert (0);
        break;
    }

    /*  Launch the state machine. */
    grid_fsm_start (&self->fsm);
}

void grid_ws_handshake_stop (struct grid_ws_handshake *self)
{
    grid_fsm_stop (&self->fsm);
}

static void grid_ws_handshake_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_ws_handshake *handshaker;

    handshaker = grid_cont (self, struct grid_ws_handshake, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        grid_timer_stop (&handshaker->timer);
        handshaker->state = GRID_WS_HANDSHAKE_STATE_STOPPING;
    }
    if (grid_slow (handshaker->state == GRID_WS_HANDSHAKE_STATE_STOPPING)) {
        if (!grid_timer_isidle (&handshaker->timer))
            return;
        handshaker->state = GRID_WS_HANDSHAKE_STATE_IDLE;
        grid_fsm_stopped (&handshaker->fsm, GRID_WS_HANDSHAKE_STOPPED);
        return;
    }

    grid_fsm_bad_state (handshaker->state, src, type);
}

static int grid_ws_match_token (const char* token, const char **subj,
    int case_insensitive, int ignore_leading_sp)
{
    const char *pos;

    grid_assert (token && *subj);

    pos = *subj;

    if (ignore_leading_sp) {
        while (*pos == '\x20' && *pos) {
            pos++;
        }
    }

    if (case_insensitive) {
        while (*token && *pos) {
            if (tolower (*token) != tolower (*pos))
                return GRID_WS_HANDSHAKE_NOMATCH;
            token++;
            pos++;
        }
    }
    else {
        while (*token && *pos) {
            if (*token != *pos)
                return GRID_WS_HANDSHAKE_NOMATCH;
            token++;
            pos++;
        }
    }

    /*  Encountered end of subject before matching completed. */
    if (!*pos && *token)
        return GRID_WS_HANDSHAKE_NOMATCH;

    /*  Entire token has been matched. */
    grid_assert (!*token);

    /*  On success, advance subject position. */
    *subj = pos;

    return GRID_WS_HANDSHAKE_MATCH;
}

static int grid_ws_match_value (const char* termseq, const char **subj,
    int ignore_leading_sp, int ignore_trailing_sp, const char **addr,
    size_t* const len)
{
    const char *start;
    const char *end;

    grid_assert (termseq && *subj);

    start = *subj;
    if (addr)
        *addr = NULL;
    if (len)
        *len = 0;

    /*  Find first occurence of termination sequence. */
    end = strstr (start, termseq);

    /*  Was a termination sequence found? */
    if (end) {
        *subj = end + strlen (termseq);
    }
    else {
        return GRID_WS_HANDSHAKE_NOMATCH;
    }
        
    if (ignore_leading_sp) {
        while (*start == '\x20' && start < end) {
            start++;
        }
    }

    if (addr)
        *addr = start;

    /*  In this special case, the value was "found", but is just empty or
        ignored space. */
    if (start == end)
        return GRID_WS_HANDSHAKE_MATCH;

    if (ignore_trailing_sp) {
        while (*(end - 1) == '\x20' && start < end) {
            end--;
        }
    }

    if (len)
        *len = end - start;

    return GRID_WS_HANDSHAKE_MATCH;
}

static int grid_ws_validate_value (const char* expected, const char *subj,
    size_t subj_len, int case_insensitive)
{
    if (strlen (expected) != subj_len)
        return GRID_WS_HANDSHAKE_NOMATCH;

    if (case_insensitive) {
        while (*expected && *subj) {
            if (tolower (*expected) != tolower (*subj))
                return GRID_WS_HANDSHAKE_NOMATCH;
            expected++;
            subj++;
        }
    }
    else {
        while (*expected && *subj) {
            if (*expected != *subj)
                return GRID_WS_HANDSHAKE_NOMATCH;
            expected++;
            subj++;
        }
    }

    return GRID_WS_HANDSHAKE_MATCH;
}

static void grid_ws_handshake_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_ws_handshake *handshaker;

    unsigned i;

    handshaker = grid_cont (self, struct grid_ws_handshake, fsm);

    switch (handshaker->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case GRID_WS_HANDSHAKE_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_assert (handshaker->recv_pos == 0);
                grid_assert (handshaker->recv_len >= GRID_WS_HANDSHAKE_TERMSEQ_LEN);

                grid_timer_start (&handshaker->timer, handshaker->timeout);

                switch (handshaker->mode) {
                case GRID_WS_CLIENT:
                    /*  Send opening handshake to server. */
                    grid_assert (handshaker->recv_len <=
                        sizeof (handshaker->response));
                    handshaker->state = GRID_WS_HANDSHAKE_STATE_CLIENT_SEND;
                    grid_ws_handshake_client_request (handshaker);
                    return;
                case GRID_WS_SERVER:
                    /*  Begin receiving opening handshake from client. */
                    grid_assert (handshaker->recv_len <=
                        sizeof (handshaker->opening_hs));
                    handshaker->state = GRID_WS_HANDSHAKE_STATE_SERVER_RECV;
                    grid_usock_recv (handshaker->usock, handshaker->opening_hs,
                        handshaker->recv_len, NULL);
                    return;
                default:
                    /*  Unexpected mode. */
                    grid_assert (0);
                    return;
                }

            default:
                grid_fsm_bad_action (handshaker->state, src, type);
            }

        default:
            grid_fsm_bad_source (handshaker->state, src, type);
        }

/******************************************************************************/
/*  SERVER_RECV state.                                                        */
/******************************************************************************/
    case GRID_WS_HANDSHAKE_STATE_SERVER_RECV:
        switch (src) {

        case GRID_WS_HANDSHAKE_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_RECEIVED:
                /*  Parse bytes received thus far. */
                switch (grid_ws_handshake_parse_client_opening (handshaker)) {
                case GRID_WS_HANDSHAKE_INVALID:
                    /*  Opening handshake parsed successfully but does not
                        contain valid values. Respond failure to client. */
                    handshaker->state = GRID_WS_HANDSHAKE_STATE_SERVER_REPLY;
                    grid_ws_handshake_server_reply (handshaker);
                    return;
                case GRID_WS_HANDSHAKE_VALID:
                    /*  Opening handshake parsed successfully, and is valid.
                        Respond success to client. */
                    handshaker->state = GRID_WS_HANDSHAKE_STATE_SERVER_REPLY;
                    grid_ws_handshake_server_reply (handshaker);
                    return;
                case GRID_WS_HANDSHAKE_RECV_MORE:
                    /*  Not enough bytes have been received to determine
                        validity; remain in the receive state, and retrieve
                        more bytes from client. */
                    handshaker->recv_pos += handshaker->recv_len;

                    /*  Validate the previous recv operation. */
                    grid_assert (handshaker->recv_pos <
                        sizeof (handshaker->opening_hs));

                    /*  Ensure we can back-track at least the length of the
                        termination sequence to determine how many bytes to
                        receive on the next retry. This is an assertion, not
                        a conditional, since under no condition is it
                        necessary to initially receive so few bytes. */
                    grid_assert (handshaker->recv_pos >=
                        (int) GRID_WS_HANDSHAKE_TERMSEQ_LEN);

                    /*  We only compare if we have at least one byte to
                        compare against.  When i drops to zero, it means
                        we don't have any bytes to match against, and it is
                        automatically true. */
                    for (i = GRID_WS_HANDSHAKE_TERMSEQ_LEN; i > 0; i--) {
                        if (memcmp (GRID_WS_HANDSHAKE_TERMSEQ,
                            handshaker->opening_hs + handshaker->recv_pos - i,
                            i) == 0) {
                            break;
                        }
                    }

                    grid_assert (i < GRID_WS_HANDSHAKE_TERMSEQ_LEN);

                    handshaker->recv_len = GRID_WS_HANDSHAKE_TERMSEQ_LEN - i;

                    /*  In the unlikely case the client would overflow what we
                        assumed was a sufficiently-large buffer to receive the
                        handshake, we fail the client. */
                    if (handshaker->recv_len + handshaker->recv_pos >
                        sizeof (handshaker->opening_hs)) {
                        handshaker->response_code =
                            GRID_WS_HANDSHAKE_RESPONSE_TOO_BIG;
                        handshaker->state =
                            GRID_WS_HANDSHAKE_STATE_SERVER_REPLY;
                        grid_ws_handshake_server_reply (handshaker);
                    }
                    else {
                        handshaker->retries++;
                        grid_usock_recv (handshaker->usock,
                            handshaker->opening_hs + handshaker->recv_pos,
                            handshaker->recv_len, NULL);
                    }
                    return;
                default:
                    grid_fsm_error ("Unexpected handshake result",
                        handshaker->state, src, type);
                }
                return;
            case GRID_USOCK_SHUTDOWN:
                /*  Ignore it and wait for ERROR event. */
                return;
            case GRID_USOCK_ERROR:
                grid_timer_stop (&handshaker->timer);
                handshaker->state = GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_ERROR;
                return;
            default:
                grid_fsm_bad_action (handshaker->state, src, type);
            }

        case GRID_WS_HANDSHAKE_SRC_TIMER:
            switch (type) {
            case GRID_TIMER_TIMEOUT:
                grid_timer_stop (&handshaker->timer);
                handshaker->state = GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_ERROR;
                return;
            default:
                grid_fsm_bad_action (handshaker->state, src, type);
            }

        default:
            grid_fsm_bad_source (handshaker->state, src, type);
        }

/******************************************************************************/
/*  SERVER_REPLY state.                                                       */
/******************************************************************************/
    case GRID_WS_HANDSHAKE_STATE_SERVER_REPLY:
        switch (src) {

        case GRID_WS_HANDSHAKE_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SENT:
                /*  As per RFC 6455 4.2.2, the handshake is now complete
                    and the connection is immediately ready for send/recv. */
                grid_timer_stop (&handshaker->timer);
                handshaker->state = GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_DONE;
            case GRID_USOCK_SHUTDOWN:
                /*  Ignore it and wait for ERROR event. */
                return;
            case GRID_USOCK_ERROR:
                grid_timer_stop (&handshaker->timer);
                handshaker->state = GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_ERROR;
                return;
            default:
                grid_fsm_bad_action (handshaker->state, src, type);
            }

        case GRID_WS_HANDSHAKE_SRC_TIMER:
            switch (type) {
            case GRID_TIMER_TIMEOUT:
                grid_timer_stop (&handshaker->timer);
                handshaker->state = GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_ERROR;
                return;
            default:
                grid_fsm_bad_action (handshaker->state, src, type);
            }

        default:
            grid_fsm_bad_source (handshaker->state, src, type);
        }

/******************************************************************************/
/*  CLIENT_SEND state.                                                        */
/******************************************************************************/
    case GRID_WS_HANDSHAKE_STATE_CLIENT_SEND:
        switch (src) {

        case GRID_WS_HANDSHAKE_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SENT:
                handshaker->state = GRID_WS_HANDSHAKE_STATE_CLIENT_RECV;
                grid_usock_recv (handshaker->usock, handshaker->response,
                    handshaker->recv_len, NULL);
                return;
            case GRID_USOCK_SHUTDOWN:
                /*  Ignore it and wait for ERROR event. */
                return;
            case GRID_USOCK_ERROR:
                grid_timer_stop (&handshaker->timer);
                handshaker->state = GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_ERROR;
                return;
            default:
                grid_fsm_bad_action (handshaker->state, src, type);
            }

        case GRID_WS_HANDSHAKE_SRC_TIMER:
            switch (type) {
            case GRID_TIMER_TIMEOUT:
                grid_timer_stop (&handshaker->timer);
                handshaker->state = GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_ERROR;
                return;
            default:
                grid_fsm_bad_action (handshaker->state, src, type);
            }

        default:
            grid_fsm_bad_source (handshaker->state, src, type);
        }

/******************************************************************************/
/*  CLIENT_RECV state.                                                        */
/******************************************************************************/
    case GRID_WS_HANDSHAKE_STATE_CLIENT_RECV:
        switch (src) {

        case GRID_WS_HANDSHAKE_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_RECEIVED:
                /*  Parse bytes received thus far. */
                switch (grid_ws_handshake_parse_server_response (handshaker)) {
                case GRID_WS_HANDSHAKE_INVALID:
                    /*  Opening handshake parsed successfully but does not
                        contain valid values. Fail connection. */
                        grid_timer_stop (&handshaker->timer);
                        handshaker->state =
                            GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_ERROR;
                    return;
                case GRID_WS_HANDSHAKE_VALID:
                    /*  As per RFC 6455 4.2.2, the handshake is now complete
                        and the connection is immediately ready for send/recv. */
                    grid_timer_stop (&handshaker->timer);
                    handshaker->state = GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_DONE;
                    return;
                case GRID_WS_HANDSHAKE_RECV_MORE:
                    /*  Not enough bytes have been received to determine
                        validity; remain in the receive state, and retrieve
                        more bytes from client. */
                    handshaker->recv_pos += handshaker->recv_len;

                    /*  Validate the previous recv operation. */
                    grid_assert (handshaker->recv_pos <
                        sizeof (handshaker->response));

                    /*  Ensure we can back-track at least the length of the
                        termination sequence to determine how many bytes to
                        receive on the next retry. This is an assertion, not
                        a conditional, since under no condition is it
                        necessary to initially receive so few bytes. */
                    grid_assert (handshaker->recv_pos >=
                        (int) GRID_WS_HANDSHAKE_TERMSEQ_LEN);

                    /*  If i goes to 0, it no need to compare. */
                    for (i = GRID_WS_HANDSHAKE_TERMSEQ_LEN; i > 0; i--) {
                        if (memcmp (GRID_WS_HANDSHAKE_TERMSEQ,
                            handshaker->response + handshaker->recv_pos - i,
                            i) == 0) {
                            break;
                        }
                    }

                    grid_assert (i < GRID_WS_HANDSHAKE_TERMSEQ_LEN);

                    handshaker->recv_len = GRID_WS_HANDSHAKE_TERMSEQ_LEN - i;

                    /*  In the unlikely case the client would overflow what we
                        assumed was a sufficiently-large buffer to receive the
                        handshake, we fail the connection. */
                    if (handshaker->recv_len + handshaker->recv_pos >
                        sizeof (handshaker->response)) {
                        grid_timer_stop (&handshaker->timer);
                        handshaker->state =
                            GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_ERROR;
                    }
                    else {
                        handshaker->retries++;
                        grid_usock_recv (handshaker->usock,
                            handshaker->response + handshaker->recv_pos,
                            handshaker->recv_len, NULL);
                    }
                    return;
                default:
                    grid_fsm_error ("Unexpected handshake result",
                        handshaker->state, src, type);
                }
                return;
            case GRID_USOCK_SHUTDOWN:
                /*  Ignore it and wait for ERROR event. */
                return;
            case GRID_USOCK_ERROR:
                grid_timer_stop (&handshaker->timer);
                handshaker->state = GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_ERROR;
                return;
            default:
                grid_fsm_bad_action (handshaker->state, src, type);
            }

        case GRID_WS_HANDSHAKE_SRC_TIMER:
            switch (type) {
            case GRID_TIMER_TIMEOUT:
                grid_timer_stop (&handshaker->timer);
                handshaker->state = GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_ERROR;
                return;
            default:
                grid_fsm_bad_action (handshaker->state, src, type);
            }

        default:
            grid_fsm_bad_source (handshaker->state, src, type);
        }

/******************************************************************************/
/*  HANDSHAKE_SENT state.                                                     */
/******************************************************************************/
    case GRID_WS_HANDSHAKE_STATE_HANDSHAKE_SENT:
        switch (src) {

        case GRID_WS_HANDSHAKE_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SENT:
                /*  As per RFC 6455 4.2.2, the handshake is now complete
                    and the connection is immediately ready for send/recv. */
                grid_timer_stop (&handshaker->timer);
                handshaker->state = GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_DONE;
                return;
            case GRID_USOCK_SHUTDOWN:
                /*  Ignore it and wait for ERROR event. */
                return;
            case GRID_USOCK_ERROR:
                grid_timer_stop (&handshaker->timer);
                handshaker->state = GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_ERROR;
                return;
            default:
                grid_fsm_bad_action (handshaker->state, src, type);
            }

        case GRID_WS_HANDSHAKE_SRC_TIMER:
            switch (type) {
            case GRID_TIMER_TIMEOUT:
                grid_timer_stop (&handshaker->timer);
                handshaker->state = GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_ERROR;
                return;
            default:
                grid_fsm_bad_action (handshaker->state, src, type);
            }

        default:
            grid_fsm_bad_source (handshaker->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_TIMER_ERROR state.                                               */
/******************************************************************************/
    case GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_ERROR:
        switch (src) {

        case GRID_WS_HANDSHAKE_SRC_USOCK:
            /*  Ignore. The only circumstance the client would send bytes is
                to notify the server it is closing the connection. Wait for the
                socket to eventually error. */
            return;

        case GRID_WS_HANDSHAKE_SRC_TIMER:
            switch (type) {
            case GRID_TIMER_STOPPED:
                grid_ws_handshake_leave (handshaker, GRID_WS_HANDSHAKE_ERROR);
                return;
            default:
                grid_fsm_bad_action (handshaker->state, src, type);
            }

        default:
            grid_fsm_bad_source (handshaker->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_TIMER_DONE state.                                                */
/******************************************************************************/
    case GRID_WS_HANDSHAKE_STATE_STOPPING_TIMER_DONE:
        switch (src) {

        case GRID_WS_HANDSHAKE_SRC_USOCK:
            /*  Ignore. The only circumstance the client would send bytes is
                to notify the server it is closing the connection. Wait for the
                socket to eventually error. */
            return;

        case GRID_WS_HANDSHAKE_SRC_TIMER:
            switch (type) {
            case GRID_TIMER_STOPPED:
                grid_ws_handshake_leave (handshaker, GRID_WS_HANDSHAKE_OK);
                return;
            default:
                grid_fsm_bad_action (handshaker->state, src, type);
            }

        default:
            grid_fsm_bad_source (handshaker->state, src, type);
        }

/******************************************************************************/
/*  DONE state.                                                               */
/*  The header exchange was either done successfully of failed. There's       */
/*  nothing that can be done in this state except stopping the object.        */
/******************************************************************************/
    case GRID_WS_HANDSHAKE_STATE_DONE:
        grid_fsm_bad_source (handshaker->state, src, type);

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (handshaker->state, src, type);
    }
}

/******************************************************************************/
/*  State machine actions.                                                    */
/******************************************************************************/

static void grid_ws_handshake_leave (struct grid_ws_handshake *self, int rc)
{
    grid_usock_swap_owner (self->usock, &self->usock_owner);
    self->usock = NULL;
    self->usock_owner.src = -1;
    self->usock_owner.fsm = NULL;
    self->state = GRID_WS_HANDSHAKE_STATE_DONE;
    grid_fsm_raise (&self->fsm, &self->done, rc);
}

static int grid_ws_handshake_parse_client_opening (struct grid_ws_handshake *self)
{
    /*  As per RFC 6455 section 1.7, this parser is not intended to be a
        general-purpose parser for arbitrary HTTP headers. As with the design
        philosophy of gridmq, application-specific exchanges are better
        reserved for accepted connections, not as fields within these
        headers. */

    int rc;
    const char *pos;
    unsigned i;

    /*  Guarantee that a NULL terminator exists to enable treating this
        recv buffer like a string. */
    grid_assert (memchr (self->opening_hs, '\0', sizeof (self->opening_hs)));

    /*  Having found the NULL terminator, from this point forward string
        functions may be used. */
    grid_assert (strlen (self->opening_hs) < sizeof (self->opening_hs));

    pos = self->opening_hs;

    /*  Is the opening handshake from the client fully received? */
    if (!strstr (pos, GRID_WS_HANDSHAKE_TERMSEQ))
        return GRID_WS_HANDSHAKE_RECV_MORE;

    self->host = NULL;
    self->origin = NULL;
    self->key = NULL;
    self->upgrade = NULL;
    self->conn = NULL;
    self->version = NULL;
    self->protocol = NULL;
    self->uri = NULL;

    self->host_len = 0;
    self->origin_len = 0;
    self->key_len = 0;
    self->upgrade_len = 0;
    self->cogrid_len = 0;
    self->version_len = 0;
    self->protocol_len = 0;
    self->uri_len = 0;

    /*  This function, if generating a return value that triggers
        a response to the client, should replace this sentinel value
        with a proper response code. */
    self->response_code = GRID_WS_HANDSHAKE_RESPONSE_NULL;

    /*  RFC 7230 3.1.1 Request Line: HTTP Method
        Note requirement of one space and case sensitivity. */
    if (!grid_ws_match_token ("GET\x20", &pos, 0, 0))
        return GRID_WS_HANDSHAKE_RECV_MORE;

    /*  RFC 7230 3.1.1 Request Line: Requested Resource. */
    if (!grid_ws_match_value ("\x20", &pos, 0, 0, &self->uri, &self->uri_len))
        return GRID_WS_HANDSHAKE_RECV_MORE;

    /*  RFC 7230 3.1.1 Request Line: HTTP version. Note case sensitivity. */
    if (!grid_ws_match_token ("HTTP/1.1", &pos, 0, 0))
        return GRID_WS_HANDSHAKE_RECV_MORE;
    if (!grid_ws_match_token (GRID_WS_HANDSHAKE_CRLF, &pos, 0, 0))
        return GRID_WS_HANDSHAKE_RECV_MORE;

    /*  It's expected the current position is now at the first
        header field. Match them one by one. */
    while (strlen (pos))
    {
        if (grid_ws_match_token ("Host:", &pos, 1, 0)) {
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                &self->host, &self->host_len);
        }
        else if (grid_ws_match_token ("Origin:",
            &pos, 1, 0) == GRID_WS_HANDSHAKE_MATCH) {
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                &self->origin, &self->origin_len);
        }
        else if (grid_ws_match_token ("Sec-WebSocket-Key:",
            &pos, 1, 0) == GRID_WS_HANDSHAKE_MATCH) {
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                &self->key, &self->key_len);
        }
        else if (grid_ws_match_token ("Upgrade:",
            &pos, 1, 0) == GRID_WS_HANDSHAKE_MATCH) {
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                &self->upgrade, &self->upgrade_len);
        }
        else if (grid_ws_match_token ("Connection:",
            &pos, 1, 0) == GRID_WS_HANDSHAKE_MATCH) {
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                &self->conn, &self->cogrid_len);
        }
        else if (grid_ws_match_token ("Sec-WebSocket-Version:",
            &pos, 1, 0) == GRID_WS_HANDSHAKE_MATCH) {
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                &self->version, &self->version_len);
        }
        else if (grid_ws_match_token ("Sec-WebSocket-Protocol:",
            &pos, 1, 0) == GRID_WS_HANDSHAKE_MATCH) {
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                &self->protocol, &self->protocol_len);
        }
        else if (grid_ws_match_token ("Sec-WebSocket-Extensions:",
            &pos, 1, 0) == GRID_WS_HANDSHAKE_MATCH) {
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                &self->extensions, &self->extensions_len);
        }
        else if (grid_ws_match_token (GRID_WS_HANDSHAKE_CRLF,
            &pos, 1, 0) == GRID_WS_HANDSHAKE_MATCH) {
            /*  Exit loop since all headers are parsed. */
            break;
        }
        else {
            /*  Skip unknown headers. */
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                NULL, NULL);
        }

        if (rc != GRID_WS_HANDSHAKE_MATCH)
            return GRID_WS_HANDSHAKE_RECV_MORE;
    }

    /*  Validate the opening handshake is now fully parsed. Additionally,
        as per RFC 6455 section 4.1, the client should not send additional data
        after the opening handshake, so this assertion validates upstream recv
        logic prevented this case. */
    grid_assert (strlen (pos) == 0);

    /*  TODO: protocol expectations below this point are hard-coded here as
        an initial design decision. Perhaps in the future these values should
        be settable via compile time (or run-time socket) options? */

    /*  These header fields are required as per RFC 6455 section 4.1. */
    if (!self->host || !self->upgrade || !self->conn ||
        !self->key || !self->version) {
        self->response_code = GRID_WS_HANDSHAKE_RESPONSE_WSPROTO;
        return GRID_WS_HANDSHAKE_INVALID;
    }

    /*  RFC 6455 section 4.2.1.6 (version December 2011). */
    if (grid_ws_validate_value ("13", self->version,
        self->version_len, 1) != GRID_WS_HANDSHAKE_MATCH) {
        self->response_code = GRID_WS_HANDSHAKE_RESPONSE_WSVERSION;
        return GRID_WS_HANDSHAKE_INVALID;
    }

    /*  RFC 6455 section 4.2.1.3 (version December 2011). */
    if (grid_ws_validate_value ("websocket", self->upgrade,
        self->upgrade_len, 1) != GRID_WS_HANDSHAKE_MATCH) {
        self->response_code = GRID_WS_HANDSHAKE_RESPONSE_WSPROTO;
        return GRID_WS_HANDSHAKE_INVALID;
    }

    /*  RFC 6455 section 4.2.1.4 (version December 2011). */
    if (grid_ws_validate_value ("Upgrade", self->conn,
        self->cogrid_len, 1) != GRID_WS_HANDSHAKE_MATCH) {
        self->response_code = GRID_WS_HANDSHAKE_RESPONSE_WSPROTO;
        return GRID_WS_HANDSHAKE_INVALID;
    }

    /*  At this point, client meets RFC 6455 compliance for opening handshake.
        Now it's time to check gridmq-imposed required handshake values. */
    if (self->protocol) {
        /*  Ensure the client SP is a compatible socket type. */
        for (i = 0; i < GRID_WS_HANDSHAKE_SP_MAP_LEN; i++) {
            if (grid_ws_validate_value (GRID_WS_HANDSHAKE_SP_MAP [i].ws_sp,
                self->protocol, self->protocol_len, 1)) {

                if (self->pipebase->sock->socktype->protocol ==
                    GRID_WS_HANDSHAKE_SP_MAP [i].server) {
                    self->response_code = GRID_WS_HANDSHAKE_RESPONSE_OK;
                    return GRID_WS_HANDSHAKE_VALID;
                }
                else {
                    self->response_code = GRID_WS_HANDSHAKE_RESPONSE_NOTPEER;
                    return GRID_WS_HANDSHAKE_INVALID;
                }
                break;
            }
        }

        self->response_code = GRID_WS_HANDSHAKE_RESPONSE_UNKNOWNTYPE;
        return GRID_WS_HANDSHAKE_INVALID;
    }
    else {
        /*  Be permissive and generous here, assuming that if a protocol is
            not explicitly declared, PAIR is presumed. This enables
            interoperability with non-gridmq remote peers, nominally by
            making the local socket PAIR type. For any other local
            socket type, we expect connection to be rejected as
            incompatible if the header is not specified. */

        if (grid_pipebase_ispeer (self->pipebase, GRID_PAIR)) {
            self->response_code = GRID_WS_HANDSHAKE_RESPONSE_OK;
            return GRID_WS_HANDSHAKE_VALID;
        }
        else {
            self->response_code = GRID_WS_HANDSHAKE_RESPONSE_NOTPEER;
            return GRID_WS_HANDSHAKE_INVALID;
        }
    }
}

static int grid_ws_handshake_parse_server_response (struct grid_ws_handshake *self)
{
    /*  As per RFC 6455 section 1.7, this parser is not intended to be a
        general-purpose parser for arbitrary HTTP headers. As with the design
        philosophy of gridmq, application-specific exchanges are better
        reserved for accepted connections, not as fields within these
        headers. */

    int rc;
    const char *pos;

    /*  Guarantee that a NULL terminator exists to enable treating this
        recv buffer like a string. The lack of such would indicate a failure
        upstream to catch a buffer overflow. */
    grid_assert (memchr (self->response, '\0', sizeof (self->response)));

    /*  Having found the NULL terminator, from this point forward string
        functions may be used. */
    grid_assert (strlen (self->response) < sizeof (self->response));

    pos = self->response;

    /*  Is the response from the server fully received? */
    if (!strstr (pos, GRID_WS_HANDSHAKE_TERMSEQ))
        return GRID_WS_HANDSHAKE_RECV_MORE;

    self->status_code = NULL;
    self->reason_phrase = NULL;
    self->server = NULL;
    self->accept_key = NULL;
    self->upgrade = NULL;
    self->conn = NULL;
    self->version = NULL;
    self->protocol = NULL;

    self->status_code_len = 0;
    self->reason_phrase_len = 0;
    self->server_len = 0;
    self->accept_key_len = 0;
    self->upgrade_len = 0;
    self->cogrid_len = 0;
    self->version_len = 0;
    self->protocol_len = 0;

    /*  RFC 7230 3.1.2 Status Line: HTTP Version. */
    if (!grid_ws_match_token ("HTTP/1.1\x20", &pos, 0, 0))
        return GRID_WS_HANDSHAKE_RECV_MORE;

    /*  RFC 7230 3.1.2 Status Line: Status Code. */
    if (!grid_ws_match_value ("\x20", &pos, 0, 0, &self->status_code,
        &self->status_code_len))
        return GRID_WS_HANDSHAKE_RECV_MORE;

    /*  RFC 7230 3.1.2 Status Line: Reason Phrase. */
    if (!grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 0, 0,
        &self->reason_phrase, &self->reason_phrase_len))
        return GRID_WS_HANDSHAKE_RECV_MORE;

    /*  It's expected the current position is now at the first
        header field. Match them one by one. */
    while (strlen (pos))
    {
        if (grid_ws_match_token ("Server:", &pos, 1, 0)) {
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                &self->server, &self->server_len);
        }
        else if (grid_ws_match_token ("Sec-WebSocket-Accept:",
            &pos, 1, 0) == GRID_WS_HANDSHAKE_MATCH) {
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                &self->accept_key, &self->accept_key_len);
        }
        else if (grid_ws_match_token ("Upgrade:",
            &pos, 1, 0) == GRID_WS_HANDSHAKE_MATCH) {
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                &self->upgrade, &self->upgrade_len);
        }
        else if (grid_ws_match_token ("Connection:",
            &pos, 1, 0) == GRID_WS_HANDSHAKE_MATCH) {
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                &self->conn, &self->cogrid_len);
        }
        else if (grid_ws_match_token ("Sec-WebSocket-Version-Server:",
            &pos, 1, 0) == GRID_WS_HANDSHAKE_MATCH) {
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                &self->version, &self->version_len);
        }
        else if (grid_ws_match_token ("Sec-WebSocket-Protocol-Server:",
            &pos, 1, 0) == GRID_WS_HANDSHAKE_MATCH) {
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                &self->protocol, &self->protocol_len);
        }
        else if (grid_ws_match_token ("Sec-WebSocket-Extensions:",
            &pos, 1, 0) == GRID_WS_HANDSHAKE_MATCH) {
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                &self->extensions, &self->extensions_len);
        }
        else if (grid_ws_match_token (GRID_WS_HANDSHAKE_CRLF,
            &pos, 1, 0) == GRID_WS_HANDSHAKE_MATCH) {
            /*  Exit loop since all headers are parsed. */
            break;
        }
        else {
            /*  Skip unknown headers. */
            rc = grid_ws_match_value (GRID_WS_HANDSHAKE_CRLF, &pos, 1, 1,
                NULL, NULL);
        }

        if (rc != GRID_WS_HANDSHAKE_MATCH)
            return GRID_WS_HANDSHAKE_RECV_MORE;
    }

    /*  Validate the opening handshake is now fully parsed. Additionally,
        as per RFC 6455 section 4.1, the client should not send additional data
        after the opening handshake, so this assertion validates upstream recv
        logic prevented this case. */
    grid_assert (strlen (pos) == 0);

    /*  TODO: protocol expectations below this point are hard-coded here as
        an initial design decision. Perhaps in the future these values should
        be settable via compile time (or run-time socket) options? */

    /*  These header fields are required as per RFC 6455 4.2.2. */
    if (!self->status_code || !self->upgrade || !self->conn ||
        !self->accept_key)
        return GRID_WS_HANDSHAKE_INVALID;

    /*  TODO: Currently, we only handle a successful connection upgrade.
        Anything else is treated as a failed connection.
        Consider handling other scenarios like 3xx redirects. */
    if (grid_ws_validate_value ("101", self->status_code,
        self->status_code_len, 1) != GRID_WS_HANDSHAKE_MATCH)
        return GRID_WS_HANDSHAKE_INVALID;

    /*  RFC 6455 section 4.2.2.5.2 (version December 2011). */
    if (grid_ws_validate_value ("websocket", self->upgrade,
        self->upgrade_len, 1) != GRID_WS_HANDSHAKE_MATCH)
        return GRID_WS_HANDSHAKE_INVALID;

    /*  RFC 6455 section 4.2.2.5.3 (version December 2011). */
    if (grid_ws_validate_value ("Upgrade", self->conn,
        self->cogrid_len, 1) != GRID_WS_HANDSHAKE_MATCH)
        return GRID_WS_HANDSHAKE_INVALID;

    /*  RFC 6455 section 4.2.2.5.4 (version December 2011). */
    if (grid_ws_validate_value (self->expected_accept_key, self->accept_key,
        self->accept_key_len, 1) != GRID_WS_HANDSHAKE_MATCH)
        return GRID_WS_HANDSHAKE_INVALID;

    /*  Server response meets RFC 6455 compliance for opening handshake. */
    return GRID_WS_HANDSHAKE_VALID;
}

static void grid_ws_handshake_client_request (struct grid_ws_handshake *self)
{
    struct grid_iovec open_request;
    size_t encoded_key_len;
    int rc;
    unsigned i;

    /*  Generate random 16-byte key as per RFC 6455 4.1 */
    uint8_t rand_key [16];

    /*  Known length required to base64 encode above random key plus
        string NULL terminator. */
    char encoded_key [24 + 1];

    grid_random_generate (rand_key, sizeof (rand_key));

    rc = grid_base64_encode (rand_key, sizeof (rand_key),
        encoded_key, sizeof (encoded_key));

    encoded_key_len = strlen (encoded_key);

    grid_assert (encoded_key_len == sizeof (encoded_key) - 1);

    /*  Pre-calculated expected Accept Key value as per
        RFC 6455 section 4.2.2.5.4 (version December 2011). */
    rc = grid_ws_handshake_hash_key (encoded_key, encoded_key_len,
        self->expected_accept_key, sizeof (self->expected_accept_key));

    grid_assert (rc == GRID_WS_HANDSHAKE_ACCEPT_KEY_LEN);

    /*  Lookup SP header value. */
    for (i = 0; i < GRID_WS_HANDSHAKE_SP_MAP_LEN; i++) {
        if (GRID_WS_HANDSHAKE_SP_MAP [i].client ==
            self->pipebase->sock->socktype->protocol) {
            break;
        }
    }

    /*  Guarantee that the socket type was found in the map. */
    grid_assert (i < GRID_WS_HANDSHAKE_SP_MAP_LEN);

    sprintf (self->opening_hs,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: %s\r\n\r\n",
        self->resource, self->remote_host, encoded_key,
        GRID_WS_HANDSHAKE_SP_MAP[i].ws_sp);

    open_request.iov_len = strlen (self->opening_hs);
    open_request.iov_base = self->opening_hs;

    grid_usock_send (self->usock, &open_request, 1);
}

static void grid_ws_handshake_server_reply (struct grid_ws_handshake *self)
{
    struct grid_iovec response;
    char *code;
    char *version;
    char *protocol;
    int rc;

    /*  Allow room for NULL terminator. */
    char accept_key [GRID_WS_HANDSHAKE_ACCEPT_KEY_LEN + 1];

    memset (self->response, 0, sizeof (self->response));

    if (self->response_code == GRID_WS_HANDSHAKE_RESPONSE_OK) {
        /*  Upgrade connection as per RFC 6455 section 4.2.2. */
        
        rc = grid_ws_handshake_hash_key (self->key, self->key_len,
            accept_key, sizeof (accept_key));

        grid_assert (strlen (accept_key) == GRID_WS_HANDSHAKE_ACCEPT_KEY_LEN);

        protocol = grid_alloc (self->protocol_len + 1, "WebSocket protocol");
        alloc_assert (protocol);
        strncpy (protocol, self->protocol, self->protocol_len);
        protocol [self->protocol_len] = '\0';

        sprintf (self->response,
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n"
            "Sec-WebSocket-Protocol: %s\r\n\r\n",
            accept_key, protocol);

        grid_free (protocol);
    }
    else {
        /*  Fail the connection with a helpful hint. */
        switch (self->response_code) {
        case GRID_WS_HANDSHAKE_RESPONSE_TOO_BIG:
            code = "400 Opening Handshake Too Long";
            break;
        case GRID_WS_HANDSHAKE_RESPONSE_WSPROTO:
            code = "400 Cannot Have Body";
            break;
        case GRID_WS_HANDSHAKE_RESPONSE_WSVERSION:
            code = "400 Unsupported WebSocket Version";
            break;
        case GRID_WS_HANDSHAKE_RESPONSE_GRIDPROTO:
            code = "400 Missing gridmq Required Headers";
            break;
        case GRID_WS_HANDSHAKE_RESPONSE_NOTPEER:
            code = "400 Incompatible Socket Type";
            break;
        case GRID_WS_HANDSHAKE_RESPONSE_UNKNOWNTYPE:
            code = "400 Unrecognized Socket Type";
            break;
        default:
            /*  Unexpected failure response. */
            grid_assert (0);
            break;
        }

        version = grid_alloc (self->version_len + 1, "WebSocket version");
        alloc_assert (version);
        strncpy (version, self->version, self->version_len);
        version [self->version_len] = '\0';

        /*  Fail connection as per RFC 6455 4.4. */
        sprintf (self->response,
            "HTTP/1.1 %s\r\n"
            "Sec-WebSocket-Version: %s\r\n",
            code, version);

        grid_free (version);
    }

    response.iov_len = strlen (self->response);
    response.iov_base = &self->response;

    grid_usock_send (self->usock, &response, 1);

    return;
}

static int grid_ws_handshake_hash_key (const char *key, size_t key_len,
    char *hashed, size_t hashed_len)
{
    int rc;
    unsigned i;
    struct grid_sha1 hash;

    grid_sha1_init (&hash);

    for (i = 0; i < key_len; i++)
        grid_sha1_hashbyte (&hash, key [i]);

    for (i = 0; i < strlen (GRID_WS_HANDSHAKE_MAGIC_GUID); i++)
        grid_sha1_hashbyte (&hash, GRID_WS_HANDSHAKE_MAGIC_GUID [i]);

    rc = grid_base64_encode (grid_sha1_result (&hash),
        sizeof (hash.state), hashed, hashed_len);

    return rc;
}


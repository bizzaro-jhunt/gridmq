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

#ifndef GRID_WS_HANDSHAKE_INCLUDED
#define GRID_WS_HANDSHAKE_INCLUDED

#include "../../transport.h"

#include "../../aio/fsm.h"
#include "../../aio/usock.h"
#include "../../aio/timer.h"

#include "../../utils/int.h"

/*  This state machine exchanges a handshake with a WebSocket client. */

/*  Return codes of this state machine. */
#define GRID_WS_HANDSHAKE_OK 1
#define GRID_WS_HANDSHAKE_ERROR 2
#define GRID_WS_HANDSHAKE_STOPPED 3

/*  WebSocket endpoint modes that determine framing of Tx/Rx and
    Opening Handshake HTTP headers. */
#define GRID_WS_CLIENT 1
#define GRID_WS_SERVER 2

/*  A ws:// buffer for gridmq is intentionally smaller than recommendation of
    RFC 7230 3.1.1 since it neither requires nor accepts arbitrarily large
    headers. */
#define GRID_WS_HANDSHAKE_MAX_SIZE 4096

/*  WebSocket protocol tokens as per RFC 6455. */
#define GRID_WS_HANDSHAKE_MAGIC_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define GRID_WS_HANDSHAKE_CRLF "\r\n"
#define GRID_WS_HANDSHAKE_TERMSEQ "\r\n\r\n"
#define GRID_WS_HANDSHAKE_TERMSEQ_LEN strlen (GRID_WS_HANDSHAKE_TERMSEQ)

/*  Expected Accept Key length based on RFC 6455 4.2.2.5.4. */
#define GRID_WS_HANDSHAKE_ACCEPT_KEY_LEN 28

struct grid_ws_handshake {

    /*  The state machine. */
    struct grid_fsm fsm;
    int state;

    /*  Controls HTTP headers and behavior based on whether this peer is
        acting as a Client or a Server. */
    int mode;

    /*  Used to timeout opening handshake. */
    struct grid_timer timer;
    int timeout;

    /*  The underlying socket. */
    struct grid_usock *usock;

    /*  The original owner of the underlying socket. */
    struct grid_fsm_owner usock_owner;

    /*  Handle to the pipe. */
    struct grid_pipebase *pipebase;

    /*  Requested resource when acting as client. */
    const char* resource;

    /*  Remote Host in header request when acting as client. */
    const char* remote_host;

    /*  Opening handshake verbatim from client as per RFC 6455 1.3. */
    char opening_hs [GRID_WS_HANDSHAKE_MAX_SIZE];

    /*  Monitor/control the opening recv poll. */
    int retries;
    size_t recv_pos;
    size_t recv_len;

    /*  Expected handshake fields from client as per RFC 6455 4.1,
        where these pointers reference the opening_hs. */
    const char *host;
    size_t host_len;

    const char *origin;
    size_t origin_len;

    const char *key;
    size_t key_len;

    const char *upgrade;
    size_t upgrade_len;

    const char *conn;
    size_t cogrid_len;

    const char *version;
    size_t version_len;

    /*  Expected handshake fields from client required by gridmq. */
    const char *protocol;
    size_t protocol_len;

    /*  Expected handshake fields from server as per RFC 6455 4.2.2. */
    const char *server;
    size_t server_len;

    const char *accept_key;
    size_t accept_key_len;

    char expected_accept_key [GRID_WS_HANDSHAKE_ACCEPT_KEY_LEN + 1];

    const char *status_code;
    size_t status_code_len;

    const char *reason_phrase;
    size_t reason_phrase_len;

    /*  Unused, optional handshake fields. */
    const char *uri;
    size_t uri_len;
    const char *extensions;
    size_t extensions_len;

    /*  Identifies the response to be sent to client's opening handshake. */
    int response_code;

    /*  Response to send back to client. */
    char response [512];

    /*  Event fired when the state machine ends. */
    struct grid_fsm_event done;
};

/*  Structure that maps scalability protocol to corresponding
    WebSocket header values. */
struct grid_ws_sp_map {

    /*  Scalability Protocol ID for server... */
    int server;

    /*  ... and corresponding client Protocol ID */
    int client;

    /*  ... and corresponding WebSocket header field value. */
    const char* ws_sp;
};

void grid_ws_handshake_init (struct grid_ws_handshake *self, int src,
    struct grid_fsm *owner);
void grid_ws_handshake_term (struct grid_ws_handshake *self);

int grid_ws_handshake_isidle (struct grid_ws_handshake *self);
void grid_ws_handshake_start (struct grid_ws_handshake *self,
    struct grid_usock *usock, struct grid_pipebase *pipebase,
    int mode, const char *resource, const char *host);
void grid_ws_handshake_stop (struct grid_ws_handshake *self);

#endif


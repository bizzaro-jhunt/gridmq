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

#ifndef GRID_SWS_INCLUDED
#define GRID_SWS_INCLUDED

#include "../../transport.h"

#include "../../aio/fsm.h"
#include "../../aio/usock.h"

#include "ws_handshake.h"

#include "../../utils/msg.h"
#include "../../utils/list.h"

/*  This state machine handles WebSocket connection from the point where it is
    established to the point when it is broken. */

/*  Return codes of this state machine. */
#define GRID_SWS_RETURN_ERROR 1
#define GRID_SWS_RETURN_CLOSE_HANDSHAKE 2
#define GRID_SWS_RETURN_STOPPED 3

/*  WebSocket protocol header frame sizes. */
#define GRID_SWS_FRAME_SIZE_INITIAL 2
#define GRID_SWS_FRAME_SIZE_PAYLOAD_0 0
#define GRID_SWS_FRAME_SIZE_PAYLOAD_16 2
#define GRID_SWS_FRAME_SIZE_PAYLOAD_63 8
#define GRID_SWS_FRAME_SIZE_MASK 4

/*  WebSocket control bitmasks as per RFC 6455 5.2. */
#define GRID_SWS_FRAME_BITMASK_FIN 0x80
#define GRID_SWS_FRAME_BITMASK_RSV1 0x40
#define GRID_SWS_FRAME_BITMASK_RSV2 0x20
#define GRID_SWS_FRAME_BITMASK_RSV3 0x10
#define GRID_SWS_FRAME_BITMASK_OPCODE 0x0F

/*  UTF-8 validation. */
#define GRID_SWS_UTF8_MAX_CODEPOINT_LEN 4

/*  The longest possible header frame length. As per RFC 6455 5.2:
    first 2 bytes of initial framing + up to 8 bytes of additional
    extended payload length header + 4 byte mask = 14bytes
    Not all messages will use the maximum amount allocated, but
    statically allocating this buffer for convenience. */
#define GRID_SWS_FRAME_MAX_HDR_LEN 14

/*  WebSocket protocol payload length framing RFC 6455 section 5.2. */
#define GRID_SWS_PAYLOAD_MAX_LENGTH 125
#define GRID_SWS_PAYLOAD_MAX_LENGTH_16 65535
#define GRID_SWS_PAYLOAD_MAX_LENGTH_63 9223372036854775807
#define GRID_SWS_PAYLOAD_FRAME_16 0x7E
#define GRID_SWS_PAYLOAD_FRAME_63 0x7F

/*  WebSocket Close Status Code length. */
#define GRID_SWS_CLOSE_CODE_LEN 2

struct grid_sws {

    /*  The state machine. */
    struct grid_fsm fsm;
    int state;

    /*  Endpoint base. */
    struct grid_epbase *epbase;

    /*  Default message type set on outbound frames. */
    uint8_t msg_type;

    /*  Controls Tx/Rx framing based on whether this peer is acting as
        a Client or a Server. */
    int mode;

    /*  The underlying socket. */
    struct grid_usock *usock;

    /*  Child state machine to do protocol header exchange. */
    struct grid_ws_handshake handshaker;

    /*  The original owner of the underlying socket. */
    struct grid_fsm_owner usock_owner;

    /*  Pipe connecting this WebSocket connection to the gridmq core. */
    struct grid_pipebase pipebase;

    /*  Requested resource when acting as client. */
    const char* resource;

    /*  Remote Host in header request when acting as client. */
    const char* remote_host;

    /*  State of inbound state machine. */
    int instate;

    /*  Buffer used to store the framing of incoming message. */
    uint8_t inhdr [GRID_SWS_FRAME_MAX_HDR_LEN];

    /*  Parsed header frames. */
    uint8_t opcode;
    uint8_t payload_ctl;
    uint8_t masked;
    uint8_t *mask;
    size_t ext_hdr_len;
    int is_final_frame;
    int is_control_frame;

    /*  As valid fragments are being received, this flag stays true until
        the FIN bit is received. This state is also used to determine
        peer sequencing anamolies that trigger this endpoint to fail the
        connection. */
    int continuing;

    /*  When validating continuation frames of UTF-8, it may be necessary
        to buffer tail-end of the previous frame in order to continue
        validation in the case that frames are chopped on intra-code point
        boundaries. */
    uint8_t utf8_code_pt_fragment [GRID_SWS_UTF8_MAX_CODEPOINT_LEN];
    size_t utf8_code_pt_fragment_len;

    /*  Statistics on control frames. */
    int pings_sent;
    int pongs_sent;
    int pings_received;
    int pongs_received;

    /*  Fragments of message being received at the moment. */
    struct grid_list inmsg_array;
    uint8_t *inmsg_current_chunk_buf;
    size_t inmsg_current_chunk_len;
    size_t inmsg_total_size;
    int inmsg_chunks;
    uint8_t inmsg_hdr;

    /*  Control message being received at the moment. Because these can be
        interspersed between fragmented TEXT and BINARY messages, they are
        stored in this buffer so as not to interrupt the message array. */
    uint8_t inmsg_control [GRID_SWS_PAYLOAD_MAX_LENGTH];

    /*  Reason this connection is closing to send as closing handshake. */
    char fail_msg [GRID_SWS_PAYLOAD_MAX_LENGTH];
    size_t fail_msg_len;

    /*  State of the outbound state machine. */
    int outstate;

    /*  Buffer used to store the header of outgoing message. */
    uint8_t outhdr [GRID_SWS_FRAME_MAX_HDR_LEN];

    /*  Message being sent at the moment. */
    struct grid_msg outmsg;

    /*  Event raised when the state machine ends. */
    struct grid_fsm_event done;
};

/*  Scatter/gather array element type forincoming message chunks. Fragmented
    message frames are reassembled prior to notifying the user. */
struct msg_chunk {
    struct grid_list_item item;
    struct grid_chunkref chunk;
};

/*  Allocate a new message chunk, append it to message array, and return
    pointer to its buffer. */
void *grid_msg_chunk_new (size_t size, struct grid_list *msg_array);

/*  Deallocate a message chunk and remove it from array. */
void grid_msg_chunk_term (struct msg_chunk *it, struct grid_list *msg_array);

/*  Deallocate an entire message array. */
void grid_msg_array_term (struct grid_list *msg_array);

void grid_sws_init (struct grid_sws *self, int src,
    struct grid_epbase *epbase, struct grid_fsm *owner);
void grid_sws_term (struct grid_sws *self);

int grid_sws_isidle (struct grid_sws *self);
void grid_sws_start (struct grid_sws *self, struct grid_usock *usock, int mode,
    const char *resource, const char *host, uint8_t msg_type);
void grid_sws_stop (struct grid_sws *self);

#endif


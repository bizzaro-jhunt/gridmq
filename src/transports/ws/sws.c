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

#include "sws.h"
#include "../../ws.h"
#include "../../grid.h"

#include "../../utils/alloc.h"
#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/wire.h"
#include "../../utils/int.h"
#include "../../utils/attr.h"
#include "../../utils/random.h"

/*  States of the object as a whole. */
#define GRID_SWS_STATE_IDLE 1
#define GRID_SWS_STATE_HANDSHAKE 2
#define GRID_SWS_STATE_STOPPING_HANDSHAKE 3
#define GRID_SWS_STATE_ACTIVE 4
#define GRID_SWS_STATE_CLOSING_CONNECTION 5
#define GRID_SWS_STATE_BROKEN_CONNECTION 6
#define GRID_SWS_STATE_DONE 7
#define GRID_SWS_STATE_STOPPING 8

/*  Possible states of the inbound part of the object. */
#define GRID_SWS_INSTATE_RECV_HDR 1
#define GRID_SWS_INSTATE_RECV_HDREXT 2
#define GRID_SWS_INSTATE_RECV_PAYLOAD 3
#define GRID_SWS_INSTATE_RECVD_CHUNKED 4
#define GRID_SWS_INSTATE_RECVD_CONTROL 5
#define GRID_SWS_INSTATE_FAILING 6
#define GRID_SWS_INSTATE_CLOSED 7

/*  Possible states of the outbound part of the object. */
#define GRID_SWS_OUTSTATE_IDLE 1
#define GRID_SWS_OUTSTATE_SENDING 2

/*  Subordinate srcptr objects. */
#define GRID_SWS_SRC_USOCK 1
#define GRID_SWS_SRC_HANDSHAKE 2

/*  WebSocket opcode constants as per RFC 6455 5.2. */
#define GRID_WS_OPCODE_FRAGMENT 0x00
#define GRID_WS_OPCODE_TEXT GRID_WS_MSG_TYPE_TEXT
#define GRID_WS_OPCODE_BINARY GRID_WS_MSG_TYPE_BINARY
#define GRID_WS_OPCODE_UNUSED3 0x03
#define GRID_WS_OPCODE_UNUSED4 0x04
#define GRID_WS_OPCODE_UNUSED5 0x05
#define GRID_WS_OPCODE_UNUSED6 0x06
#define GRID_WS_OPCODE_UNUSED7 0x07
#define GRID_WS_OPCODE_CLOSE 0x08
#define GRID_WS_OPCODE_PING 0x09
#define GRID_WS_OPCODE_PONG 0x0A
#define GRID_WS_OPCODE_UNUSEDB 0x0B
#define GRID_WS_OPCODE_UNUSEDC 0x0C
#define GRID_WS_OPCODE_UNUSEDD 0x0D
#define GRID_WS_OPCODE_UNUSEDE 0x0E
#define GRID_WS_OPCODE_UNUSEDF 0x0F

/*  WebSocket protocol header bit masks as per RFC 6455. */
#define GRID_SWS_FRAME_BITMASK_MASKED 0x80
#define GRID_SWS_FRAME_BITMASK_NOT_MASKED 0x00
#define GRID_SWS_FRAME_BITMASK_LENGTH 0x7F

/*  WebSocket Close Status Codes (1004-1006 and 1015 are reserved). */
#define GRID_SWS_CLOSE_NORMAL 1000
#define GRID_SWS_CLOSE_GOING_AWAY 1001
#define GRID_SWS_CLOSE_ERR_PROTO 1002
#define GRID_SWS_CLOSE_ERR_WUT 1003
#define GRID_SWS_CLOSE_ERR_INVALID_FRAME 1007
#define GRID_SWS_CLOSE_ERR_POLICY 1008
#define GRID_SWS_CLOSE_ERR_TOOBIG 1009
#define GRID_SWS_CLOSE_ERR_EXTENSION 1010
#define GRID_SWS_CLOSE_ERR_SERVER 1011

/*  UTF-8 validation. */
#define GRID_SWS_UTF8_INVALID -2
#define GRID_SWS_UTF8_FRAGMENT -1

/*  Stream is a special type of pipe. Implementation of the virtual pipe API. */
static int grid_sws_send (struct grid_pipebase *self, struct grid_msg *msg);
static int grid_sws_recv (struct grid_pipebase *self, struct grid_msg *msg);
const struct grid_pipebase_vfptr grid_sws_pipebase_vfptr = {
    grid_sws_send,
    grid_sws_recv
};

/*  Private functions. */
static void grid_sws_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_sws_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);

/*  Ceases further I/O on the underlying socket and prepares to send a
    close handshake on the next receive. */
static int grid_sws_fail_conn (struct grid_sws *self, int code, char *reason);

/*  Start receiving new message chunk. */
static int grid_sws_recv_hdr (struct grid_sws *self);

/*  Mask or unmask message payload. */
static void grid_sws_mask_payload (uint8_t *payload, size_t payload_len,
    const uint8_t *mask, size_t mask_len, int *mask_start_pos);

/*  Validates incoming text chunks for UTF-8 compliance as per RFC 3629. */
static void grid_sws_validate_utf8_chunk (struct grid_sws *self);

void grid_sws_init (struct grid_sws *self, int src,
    struct grid_epbase *epbase, struct grid_fsm *owner)
{
    grid_fsm_init (&self->fsm, grid_sws_handler, grid_sws_shutdown,
        src, self, owner);
    self->state = GRID_SWS_STATE_IDLE;
    self->epbase = epbase;
    grid_ws_handshake_init (&self->handshaker,
        GRID_SWS_SRC_HANDSHAKE, &self->fsm);
    self->usock = NULL;
    self->usock_owner.src = -1;
    self->usock_owner.fsm = NULL;
    grid_pipebase_init (&self->pipebase, &grid_sws_pipebase_vfptr, epbase);
    self->instate = -1;
    grid_list_init (&self->inmsg_array);
    self->outstate = -1;
    grid_msg_init (&self->outmsg, 0);

    self->continuing = 0;

    memset (self->utf8_code_pt_fragment, 0,
        GRID_SWS_UTF8_MAX_CODEPOINT_LEN);
    self->utf8_code_pt_fragment_len = 0;

    self->pings_sent = 0;
    self->pongs_sent = 0;
    self->pings_received = 0;
    self->pongs_received = 0;

    grid_fsm_event_init (&self->done);
}

void grid_sws_term (struct grid_sws *self)
{
    grid_assert_state (self, GRID_SWS_STATE_IDLE);

    grid_fsm_event_term (&self->done);
    grid_msg_term (&self->outmsg);
    grid_msg_array_term (&self->inmsg_array);
    grid_pipebase_term (&self->pipebase);
    grid_ws_handshake_term (&self->handshaker);
    grid_fsm_term (&self->fsm);
}

int grid_sws_isidle (struct grid_sws *self)
{
    return grid_fsm_isidle (&self->fsm);
}

void grid_sws_start (struct grid_sws *self, struct grid_usock *usock, int mode,
    const char *resource, const char *host, uint8_t msg_type)
{
    /*  Take ownership of the underlying socket. */
    grid_assert (self->usock == NULL && self->usock_owner.fsm == NULL);
    self->usock_owner.src = GRID_SWS_SRC_USOCK;
    self->usock_owner.fsm = &self->fsm;
    grid_usock_swap_owner (usock, &self->usock_owner);
    self->usock = usock;
    self->mode = mode;
    self->resource = resource;
    self->remote_host = host;

    self->msg_type = msg_type;

    /*  Launch the state machine. */
    grid_fsm_start (&self->fsm);
}

void grid_sws_stop (struct grid_sws *self)
{
    grid_fsm_stop (&self->fsm);
}

void *grid_msg_chunk_new (size_t size, struct grid_list *msg_array)
{
    struct msg_chunk *self;

    self = grid_alloc (sizeof (struct msg_chunk), "msg_chunk");
    alloc_assert (self);

    grid_chunkref_init (&self->chunk, size);
    grid_list_item_init (&self->item);

    grid_list_insert (msg_array, &self->item, grid_list_end (msg_array));

    return grid_chunkref_data (&self->chunk);
}

void grid_msg_chunk_term (struct msg_chunk *it, struct grid_list *msg_array)
{
    grid_chunkref_term (&it->chunk);
    grid_list_erase (msg_array, &it->item);
    grid_list_item_term (&it->item);
    grid_free (it);
}

void grid_msg_array_term (struct grid_list *msg_array)
{
    struct grid_list_item *it;
    struct msg_chunk *ch;

    while (!grid_list_empty (msg_array)) {
        it = grid_list_begin (msg_array);
        ch = grid_cont (it, struct msg_chunk, item);
        grid_msg_chunk_term (ch, msg_array);
    }

    grid_list_term (msg_array);
}

static int grid_utf8_code_point (const uint8_t *buffer, size_t len)
{
    /*  The lack of information is considered neither valid nor invalid. */
    if (!buffer || !len)
        return GRID_SWS_UTF8_FRAGMENT;
    
    /*  RFC 3629 section 4 UTF8-1. */
    if (buffer [0] <= 0x7F)
        return 1;

    /*  0xC2, or 11000001, is the smallest conceivable multi-octet code
        point that is not an illegal overlong encoding. */
    if (buffer [0] < 0xC2)
        return GRID_SWS_UTF8_INVALID;

    /*  Largest 2-octet code point starts with 0xDF (11011111). */
    if (buffer [0] <= 0xDF) {
        if (len < 2)
            return GRID_SWS_UTF8_FRAGMENT;
        /*  Ensure continuation byte in form of 10xxxxxx */
        else if ((buffer [1] & 0xC0) != 0x80)
            return GRID_SWS_UTF8_INVALID;
        else
            return 2;
    }

    /*  RFC 3629 section 4 UTF8-3, where 0xEF is 11101111. */
    if (buffer [0] <= 0xEF) {
        /*  Fragment. */
        if (len < 2)
            return GRID_SWS_UTF8_FRAGMENT;
        /*  Illegal overlong sequence detection. */
        else if (buffer [0] == 0xE0 && (buffer [1] < 0xA0 || buffer [1] == 0x80))
            return GRID_SWS_UTF8_INVALID;
        /*  Illegal UTF-16 surrogate pair half U+D800 through U+DFFF. */
        else if (buffer [0] == 0xED && buffer [1] >= 0xA0)
            return GRID_SWS_UTF8_INVALID;
        /*  Fragment. */
        else if (len < 3)
            return GRID_SWS_UTF8_FRAGMENT;
        /*  Ensure continuation bytes 2 and 3 in form of 10xxxxxx */
        else if ((buffer [1] & 0xC0) != 0x80 || (buffer [2] & 0xC0) != 0x80)
            return GRID_SWS_UTF8_INVALID;
        else
            return 3;
    }

    /*  RFC 3629 section 4 UTF8-4, where 0xF4 is 11110100. Why
        not 11110111 to follow the pattern? Because UTF-8 encoding
        stops at 0x10FFFF as per RFC 3629. */
    if (buffer [0] <= 0xF4) {
        /*  Fragment. */
        if (len < 2)
            return GRID_SWS_UTF8_FRAGMENT;
        /*  Illegal overlong sequence detection. */
        else if (buffer [0] == 0xF0 && buffer [1] < 0x90)
            return GRID_SWS_UTF8_INVALID;
        /*  Illegal code point greater than U+10FFFF. */
        else if (buffer [0] == 0xF4 && buffer [1] >= 0x90)
            return GRID_SWS_UTF8_INVALID;
        /*  Fragment. */
        else if (len < 4)
            return GRID_SWS_UTF8_FRAGMENT;
        /*  Ensure continuation bytes 2, 3, and 4 in form of 10xxxxxx */
        else if ((buffer [1] & 0xC0) != 0x80 ||
            (buffer [2] & 0xC0) != 0x80 ||
            (buffer [3] & 0xC0) != 0x80)
            return GRID_SWS_UTF8_INVALID;
        else
            return 4;
    }

    /*  UTF-8 encoding stops at U+10FFFF and only defines up to 4-octet
        code point sequences. */
    if (buffer [0] >= 0xF5)
        return GRID_SWS_UTF8_INVALID;

    /*  Algorithm error; a case above should have been satisfied. */
    grid_assert (0);
}

static void grid_sws_mask_payload (uint8_t *payload, size_t payload_len,
    const uint8_t *mask, size_t mask_len, int *mask_start_pos)
{
    unsigned i;

    if (mask_start_pos) {
        for (i = 0; i < payload_len; i++) {
            payload [i] ^= mask [(i + *mask_start_pos) % mask_len];
        }

        *mask_start_pos = (i + *mask_start_pos) % mask_len;

        return;
    }
    else {
        for (i = 0; i < payload_len; i++) {
            payload [i] ^= mask [i % mask_len];
        }
        return;
    }
}

static int grid_sws_recv_hdr (struct grid_sws *self)
{
    if (!self->continuing) {
        grid_assert (grid_list_empty (&self->inmsg_array));

        self->inmsg_current_chunk_buf = NULL;
        self->inmsg_chunks = 0;
        self->inmsg_current_chunk_len = 0;
        self->inmsg_total_size = 0;
    }

    memset (self->inmsg_control, 0, GRID_SWS_PAYLOAD_MAX_LENGTH);
    memset (self->inhdr, 0, GRID_SWS_FRAME_MAX_HDR_LEN);
    self->instate = GRID_SWS_INSTATE_RECV_HDR;
    grid_usock_recv (self->usock, self->inhdr, GRID_SWS_FRAME_SIZE_INITIAL, NULL);

    return 0;
}

static int grid_sws_send (struct grid_pipebase *self, struct grid_msg *msg)
{
    struct grid_sws *sws;
    struct grid_iovec iov [3];
    int mask_pos;
    size_t grid_msg_size;
    size_t hdr_len;
    struct grid_cmsghdr *cmsg;
    struct grid_msghdr msghdr;
    uint8_t rand_mask [GRID_SWS_FRAME_SIZE_MASK];

    sws = grid_cont (self, struct grid_sws, pipebase);

    grid_assert_state (sws, GRID_SWS_STATE_ACTIVE);
    grid_assert (sws->outstate == GRID_SWS_OUTSTATE_IDLE);

    /*  Move the message to the local storage. */
    grid_msg_term (&sws->outmsg);
    grid_msg_mv (&sws->outmsg, msg);

    memset (sws->outhdr, 0, sizeof (sws->outhdr));

    hdr_len = GRID_SWS_FRAME_SIZE_INITIAL;

    cmsg = NULL;
    msghdr.msg_iov = NULL;
    msghdr.msg_iovlen = 0;
    msghdr.msg_controllen = grid_chunkref_size (&sws->outmsg.hdrs);

    /*  If the outgoing message has specified an opcode and control framing in
        its header, properly frame it as per RFC 6455 5.2. */
    if (msghdr.msg_controllen > 0) {
        msghdr.msg_control = grid_chunkref_data (&sws->outmsg.hdrs);
        cmsg = GRID_CMSG_FIRSTHDR (&msghdr);
        while (cmsg) {
            if (cmsg->cmsg_level == GRID_WS && cmsg->cmsg_type == GRID_WS_MSG_TYPE)
                break;
            cmsg = GRID_CMSG_NXTHDR (&msghdr, cmsg);
        }
    }

    /*  If the header does not specify an opcode, take default from option. */
    if (cmsg)
        sws->outhdr [0] = *(uint8_t *) GRID_CMSG_DATA (cmsg);
    else
        sws->outhdr [0] = sws->msg_type;

    /*  For now, enforce that outgoing messages are the final frame. */
    sws->outhdr [0] |= GRID_SWS_FRAME_BITMASK_FIN;

    grid_msg_size = grid_chunkref_size (&sws->outmsg.sphdr) +
        grid_chunkref_size (&sws->outmsg.body);

    /*  Framing WebSocket payload size in network byte order (big endian). */
    if (grid_msg_size <= GRID_SWS_PAYLOAD_MAX_LENGTH) {
        sws->outhdr [1] |= (uint8_t) grid_msg_size;
        hdr_len += GRID_SWS_FRAME_SIZE_PAYLOAD_0;
    }
    else if (grid_msg_size <= GRID_SWS_PAYLOAD_MAX_LENGTH_16) {
        sws->outhdr [1] |= GRID_SWS_PAYLOAD_FRAME_16;
        grid_puts (&sws->outhdr [hdr_len], (uint16_t) grid_msg_size);
        hdr_len += GRID_SWS_FRAME_SIZE_PAYLOAD_16;
    }
    else {
        sws->outhdr [1] |= GRID_SWS_PAYLOAD_FRAME_63;
        grid_putll (&sws->outhdr [hdr_len], (uint64_t) grid_msg_size);
        hdr_len += GRID_SWS_FRAME_SIZE_PAYLOAD_63;
    }

    if (sws->mode == GRID_WS_CLIENT) {
        sws->outhdr [1] |= GRID_SWS_FRAME_BITMASK_MASKED;

        /*  Generate 32-bit mask as per RFC 6455 5.3. */
        grid_random_generate (rand_mask, GRID_SWS_FRAME_SIZE_MASK);
        
        memcpy (&sws->outhdr [hdr_len], rand_mask, GRID_SWS_FRAME_SIZE_MASK);
        hdr_len += GRID_SWS_FRAME_SIZE_MASK;

        /*  Mask payload, beginning with header and moving to body. */
        mask_pos = 0;

        grid_sws_mask_payload (grid_chunkref_data (&sws->outmsg.sphdr),
            grid_chunkref_size (&sws->outmsg.sphdr),
            rand_mask, GRID_SWS_FRAME_SIZE_MASK, &mask_pos);

        grid_sws_mask_payload (grid_chunkref_data (&sws->outmsg.body),
            grid_chunkref_size (&sws->outmsg.body),
            rand_mask, GRID_SWS_FRAME_SIZE_MASK, &mask_pos);

    }
    else if (sws->mode == GRID_WS_SERVER) {
        sws->outhdr [1] |= GRID_SWS_FRAME_BITMASK_NOT_MASKED;
    }
    else {
        /*  Developer error; sws object was not constructed properly. */
        grid_assert (0);
    }

    /*  Start async sending. */
    iov [0].iov_base = sws->outhdr;
    iov [0].iov_len = hdr_len;
    iov [1].iov_base = grid_chunkref_data (&sws->outmsg.sphdr);
    iov [1].iov_len = grid_chunkref_size (&sws->outmsg.sphdr);
    iov [2].iov_base = grid_chunkref_data (&sws->outmsg.body);
    iov [2].iov_len = grid_chunkref_size (&sws->outmsg.body);
    grid_usock_send (sws->usock, iov, 3);

    sws->outstate = GRID_SWS_OUTSTATE_SENDING;

    /*  If a Close handshake was just sent, it's time to shut down. */
    if ((sws->outhdr [0] & GRID_SWS_FRAME_BITMASK_OPCODE) ==
        GRID_WS_OPCODE_CLOSE) {
        grid_pipebase_stop (&sws->pipebase);
        sws->state = GRID_SWS_STATE_CLOSING_CONNECTION;
    }

    return 0;
}

static int grid_sws_recv (struct grid_pipebase *self, struct grid_msg *msg)
{
    struct grid_sws *sws;
    struct grid_list_item *it;
    struct msg_chunk *ch;
    struct grid_cmsghdr *cmsg;
    uint8_t opcode_hdr;
    size_t cmsgsz;
    size_t pos;

    sws = grid_cont (self, struct grid_sws, pipebase);

    grid_assert_state (sws, GRID_SWS_STATE_ACTIVE);

    switch (sws->instate) {
    case GRID_SWS_INSTATE_RECVD_CHUNKED:

        /*  This library should not deliver fragmented messages to the application,
            so it's expected that this is the final frame. */
        grid_assert (sws->is_final_frame);

        grid_msg_init (msg, sws->inmsg_total_size);

        /*  Relay opcode to the user in order to interpret payload. */
        opcode_hdr = sws->inmsg_hdr;

        pos = 0;

        /*  Reassemble incoming message scatter array. */
        while (!grid_list_empty (&sws->inmsg_array)) {
            it = grid_list_begin (&sws->inmsg_array);
            ch = grid_cont (it, struct msg_chunk, item);
            memcpy (((uint8_t*) grid_chunkref_data (&msg->body)) + pos,
                grid_chunkref_data (&ch->chunk),
                grid_chunkref_size (&ch->chunk));
            pos += grid_chunkref_size (&ch->chunk);
            grid_msg_chunk_term (ch, &sws->inmsg_array);
        }

        grid_assert (pos == sws->inmsg_total_size);
        grid_assert (grid_list_empty (&sws->inmsg_array));

        /*  No longer collecting scatter array of incoming msg chunks. */
        sws->continuing = 0;

        grid_sws_recv_hdr (sws);

        break;

    case GRID_SWS_INSTATE_RECVD_CONTROL:

        /*  This library should not deliver fragmented messages to the user, so
        it's expected that this is the final frame. */
        grid_assert (sws->is_final_frame);

        grid_msg_init (msg, sws->inmsg_current_chunk_len);

        /*  Relay opcode to the user in order to interpret payload. */
        opcode_hdr = sws->inhdr [0];

        memcpy (((uint8_t*) grid_chunkref_data (&msg->body)),
            sws->inmsg_control, sws->inmsg_current_chunk_len);

        /*  If a closing handshake was just transferred to the application,
            discontinue continual, async receives. */
        if (sws->opcode == GRID_WS_OPCODE_CLOSE) {
            sws->instate = GRID_SWS_INSTATE_CLOSED;
        }
        else {
            grid_sws_recv_hdr (sws);
        }

        break;

    default:
        /*  Unexpected state. */
        grid_assert (0);
        break;
    }

    /*  Allocate and populate WebSocket-specific control headers. */
    cmsgsz = GRID_CMSG_SPACE (sizeof (opcode_hdr));
    grid_chunkref_init (&msg->hdrs, cmsgsz);
    cmsg = grid_chunkref_data (&msg->hdrs);
    cmsg->cmsg_level = GRID_WS;
    cmsg->cmsg_type = GRID_WS_MSG_TYPE;
    cmsg->cmsg_len = cmsgsz;
    memcpy (GRID_CMSG_DATA (cmsg), &opcode_hdr, sizeof (opcode_hdr));

    return 0;
}

static void grid_sws_validate_utf8_chunk (struct grid_sws *self)
{
    uint8_t *pos;
    int code_point_len;
    int len;

    len = self->inmsg_current_chunk_len;
    pos = self->inmsg_current_chunk_buf;

    /*  For chunked transfers, it's possible that a previous chunk was cut
        intra-code point. That partially-validated code point is reassembled
        with the beginning of the current chunk and checked. */
    if (self->utf8_code_pt_fragment_len) {

        grid_assert (self->utf8_code_pt_fragment_len <
            GRID_SWS_UTF8_MAX_CODEPOINT_LEN);

        /*  Keep adding octets from fresh buffer to previous code point
            fragment to check for validity. */
        while (len > 0) {
            self->utf8_code_pt_fragment [self->utf8_code_pt_fragment_len] = *pos;
            self->utf8_code_pt_fragment_len++;
            pos++;
            len--;

            code_point_len = grid_utf8_code_point (self->utf8_code_pt_fragment,
                self->utf8_code_pt_fragment_len);
            
            if (code_point_len > 0) {
                /*  Valid code point found; continue validating. */
                break;
            }
            else if (code_point_len == GRID_SWS_UTF8_INVALID) {
                grid_sws_fail_conn (self, GRID_SWS_CLOSE_ERR_INVALID_FRAME,
                    "Invalid UTF-8 code point split on previous frame.");
                return;
            }
            else if (code_point_len == GRID_SWS_UTF8_FRAGMENT) {
                if (self->is_final_frame) {
                    grid_sws_fail_conn (self, GRID_SWS_CLOSE_ERR_INVALID_FRAME,
                        "Truncated UTF-8 payload with invalid code point.");
                    return;
                }
                else {
                    /*  This chunk is well-formed; now recv the next chunk. */
                    grid_sws_recv_hdr (self);
                    return;
                }
            }
        }
    }

    if (self->utf8_code_pt_fragment_len >= GRID_SWS_UTF8_MAX_CODEPOINT_LEN)
        grid_assert (0);

    while (len > 0) {

        code_point_len = grid_utf8_code_point (pos, len);

        if (code_point_len > 0) {
            /*  Valid code point found; continue validating. */
            pos += code_point_len;
            len -= code_point_len;
            grid_assert (len >= 0);
            continue;
        }
        else if (code_point_len == GRID_SWS_UTF8_INVALID) {
            self->utf8_code_pt_fragment_len = 0;
            memset (self->utf8_code_pt_fragment, 0,
                GRID_SWS_UTF8_MAX_CODEPOINT_LEN);
            grid_sws_fail_conn (self, GRID_SWS_CLOSE_ERR_INVALID_FRAME,
                "Invalid UTF-8 code point in payload.");
            return;
        }
        else if (code_point_len == GRID_SWS_UTF8_FRAGMENT) {
            grid_assert (len < GRID_SWS_UTF8_MAX_CODEPOINT_LEN);
            self->utf8_code_pt_fragment_len = len;
            memcpy (self->utf8_code_pt_fragment, pos, len);
            if (self->is_final_frame) {
                grid_sws_fail_conn (self, GRID_SWS_CLOSE_ERR_INVALID_FRAME,
                    "Truncated UTF-8 payload with invalid code point.");
            }
            else {
                /*  Previous frame ended in the middle of a code point;
                    receive more. */
                grid_sws_recv_hdr (self);
            }
            return;
        }
    }

    /*  Entire buffer is well-formed. */
    grid_assert (len == 0);

    self->utf8_code_pt_fragment_len = 0;
    memset (self->utf8_code_pt_fragment, 0, GRID_SWS_UTF8_MAX_CODEPOINT_LEN);

    if (self->is_final_frame) {
        self->instate = GRID_SWS_INSTATE_RECVD_CHUNKED;
        grid_pipebase_received (&self->pipebase);
    }
    else {
        grid_sws_recv_hdr (self);
    }

    return;
}

static int grid_sws_fail_conn (struct grid_sws *self, int code, char *reason)
{
    size_t reason_len;
    size_t payload_len;
    uint8_t rand_mask [GRID_SWS_FRAME_SIZE_MASK];
    uint8_t *payload_pos;
    struct grid_iovec iov;

    grid_assert_state (self, GRID_SWS_STATE_ACTIVE);

    /*  Destroy any remnant incoming message fragments. */
    grid_msg_array_term (&self->inmsg_array);

    reason_len = strlen (reason);

    payload_len = reason_len + GRID_SWS_CLOSE_CODE_LEN;

    /*  Ensure text is short enough to also include code and framing. */
    grid_assert (payload_len <= GRID_SWS_PAYLOAD_MAX_LENGTH);

    /*  RFC 6455 section 5.5.1. */
    self->fail_msg [0] = GRID_SWS_FRAME_BITMASK_FIN | GRID_WS_OPCODE_CLOSE;

    /*  Size of the payload, which is the status code plus the reason. */
    self->fail_msg [1] = (char)payload_len;

    self->fail_msg_len = GRID_SWS_FRAME_SIZE_INITIAL;

    switch (self->mode) {
    case GRID_WS_SERVER:
        self->fail_msg [1] |= GRID_SWS_FRAME_BITMASK_NOT_MASKED;
        break;
    case GRID_WS_CLIENT:
        self->fail_msg [1] |= GRID_SWS_FRAME_BITMASK_MASKED;

        /*  Generate 32-bit mask as per RFC 6455 5.3. */
        grid_random_generate (rand_mask, GRID_SWS_FRAME_SIZE_MASK);

        memcpy (&self->fail_msg [GRID_SWS_FRAME_SIZE_INITIAL],
            rand_mask, GRID_SWS_FRAME_SIZE_MASK);

        self->fail_msg_len += GRID_SWS_FRAME_SIZE_MASK;
        break;
    default:
        /*  Developer error. */
        grid_assert (0);
    }

    payload_pos = (uint8_t*) (&self->fail_msg [self->fail_msg_len]);
    
    /*  Copy Status Code in network order (big-endian). */
    grid_puts (payload_pos, (uint16_t) code);
    self->fail_msg_len += GRID_SWS_CLOSE_CODE_LEN;

    /*  Copy Close Reason immediately following the code. */
    memcpy (payload_pos + GRID_SWS_CLOSE_CODE_LEN, reason, reason_len);

    /*  If this is a client, apply mask. */
    if (self->mode == GRID_WS_CLIENT) {
        grid_sws_mask_payload (payload_pos, payload_len,
            rand_mask, GRID_SWS_FRAME_SIZE_MASK, NULL);
    }

    self->fail_msg_len += payload_len;

    self->instate = GRID_SWS_INSTATE_CLOSED;

    /*  Stop user send/recv actions. */
    grid_pipebase_stop (&self->pipebase);

    if (self->outstate == GRID_SWS_OUTSTATE_IDLE) {
        iov.iov_base = self->fail_msg;
        iov.iov_len = self->fail_msg_len;
        grid_usock_send (self->usock, &iov, 1);
        self->outstate = GRID_SWS_OUTSTATE_SENDING;
        self->state = GRID_SWS_STATE_CLOSING_CONNECTION;
    } else {
        self->state = GRID_SWS_STATE_DONE;
        grid_fsm_raise (&self->fsm, &self->done, GRID_SWS_RETURN_CLOSE_HANDSHAKE);
    }

    return 0;
}

static void grid_sws_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_sws *sws;

    sws = grid_cont (self, struct grid_sws, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        /*  TODO: Consider sending a close code here? */
        grid_pipebase_stop (&sws->pipebase);
        grid_ws_handshake_stop (&sws->handshaker);
        sws->state = GRID_SWS_STATE_STOPPING;
    }
    if (grid_slow (sws->state == GRID_SWS_STATE_STOPPING)) {
        if (grid_ws_handshake_isidle (&sws->handshaker)) {
            grid_usock_swap_owner (sws->usock, &sws->usock_owner);
            sws->usock = NULL;
            sws->usock_owner.src = -1;
            sws->usock_owner.fsm = NULL;
            sws->state = GRID_SWS_STATE_IDLE;
            grid_fsm_stopped (&sws->fsm, GRID_SWS_RETURN_STOPPED);
            return;
        }
        return;
    }

    grid_fsm_bad_state (sws->state, src, type);
}

static void grid_sws_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_sws *sws;
    int rc;

    sws = grid_cont (self, struct grid_sws, fsm);

    switch (sws->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/******************************************************************************/
    case GRID_SWS_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                grid_ws_handshake_start (&sws->handshaker, sws->usock,
                    &sws->pipebase, sws->mode, sws->resource, sws->remote_host);
                sws->state = GRID_SWS_STATE_HANDSHAKE;
                return;
            default:
                grid_fsm_bad_action (sws->state, src, type);
            }

        default:
            grid_fsm_bad_source (sws->state, src, type);
        }

/******************************************************************************/
/*  HANDSHAKE state.                                                          */
/******************************************************************************/
    case GRID_SWS_STATE_HANDSHAKE:
        switch (src) {

        case GRID_SWS_SRC_HANDSHAKE:
            switch (type) {
            case GRID_WS_HANDSHAKE_OK:

                /*  Before moving to the active state stop the handshake
                    state machine. */
                grid_ws_handshake_stop (&sws->handshaker);
                sws->state = GRID_SWS_STATE_STOPPING_HANDSHAKE;
                return;

            case GRID_WS_HANDSHAKE_ERROR:

                /* Raise the error and move directly to the DONE state.
                   ws_handshake object will be stopped later on. */
                sws->state = GRID_SWS_STATE_DONE;
                grid_fsm_raise (&sws->fsm, &sws->done,
                    GRID_SWS_RETURN_CLOSE_HANDSHAKE);
                return;

            default:
                grid_fsm_bad_action (sws->state, src, type);
            }

        default:
            grid_fsm_bad_source (sws->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_HANDSHAKE state.                                                 */
/******************************************************************************/
    case GRID_SWS_STATE_STOPPING_HANDSHAKE:
        switch (src) {

        case GRID_SWS_SRC_HANDSHAKE:
            switch (type) {
            case GRID_WS_HANDSHAKE_STOPPED:

                 /*  Start the pipe. */
                 rc = grid_pipebase_start (&sws->pipebase);
                 if (grid_slow (rc < 0)) {
                    sws->state = GRID_SWS_STATE_DONE;
                    grid_fsm_raise (&sws->fsm, &sws->done, GRID_SWS_RETURN_ERROR);
                    return;
                 }

                 /*  Start receiving a message in asynchronous manner. */
                 grid_sws_recv_hdr (sws);

                 /*  Mark the pipe as available for sending. */
                 sws->outstate = GRID_SWS_OUTSTATE_IDLE;

                 sws->state = GRID_SWS_STATE_ACTIVE;
                 return;

            default:
                grid_fsm_bad_action (sws->state, src, type);
            }

        default:
            grid_fsm_bad_source (sws->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case GRID_SWS_STATE_ACTIVE:
        switch (src) {

        case GRID_SWS_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SENT:

                /*  The message is now fully sent. */
                grid_assert (sws->outstate == GRID_SWS_OUTSTATE_SENDING);
                sws->outstate = GRID_SWS_OUTSTATE_IDLE;
                grid_msg_term (&sws->outmsg);
                grid_msg_init (&sws->outmsg, 0);
                grid_pipebase_sent (&sws->pipebase);
                return;

            case GRID_USOCK_RECEIVED:

                switch (sws->instate) {
                case GRID_SWS_INSTATE_RECV_HDR:

                    /*  Require RSV1, RSV2, and RSV3 bits to be unset for
                        x-gridmq protocol as per RFC 6455 section 5.2. */
                    if (sws->inhdr [0] & GRID_SWS_FRAME_BITMASK_RSV1 ||
                        sws->inhdr [0] & GRID_SWS_FRAME_BITMASK_RSV2 ||
                        sws->inhdr [0] & GRID_SWS_FRAME_BITMASK_RSV3) {
                        grid_sws_fail_conn (sws, GRID_SWS_CLOSE_ERR_PROTO,
                            "RSV1, RSV2, and RSV3 must be unset.");
                        return;
                    }

                    sws->is_final_frame = sws->inhdr [0] &
                        GRID_SWS_FRAME_BITMASK_FIN;
                    sws->masked = sws->inhdr [1] &
                        GRID_SWS_FRAME_BITMASK_MASKED;

                    switch (sws->mode) {
                    case GRID_WS_SERVER:
                        /*  Require mask bit to be set from client. */
                        if (sws->masked) {
                            /*  Continue receiving header for this frame. */
                            sws->ext_hdr_len = GRID_SWS_FRAME_SIZE_MASK;
                            break;
                        }
                        else {
                            grid_sws_fail_conn (sws, GRID_SWS_CLOSE_ERR_PROTO,
                                "Server expects MASK bit to be set.");
                            return;
                        }
                    case GRID_WS_CLIENT:
                        /*  Require mask bit to be unset from server. */
                        if (sws->masked) {
                            grid_sws_fail_conn (sws, GRID_SWS_CLOSE_ERR_PROTO,
                                "Client expects MASK bit to be unset.");
                            return;
                        }
                        else {
                            /*  Continue receiving header for this frame. */
                            sws->ext_hdr_len = 0;
                            break;
                        }
                    default:
                        /*  Only two modes of this endpoint are expected. */
                        grid_assert (0);
                        return;
                    }

                    sws->opcode = sws->inhdr [0] &
                        GRID_SWS_FRAME_BITMASK_OPCODE;
                    sws->payload_ctl = sws->inhdr [1] &
                        GRID_SWS_FRAME_BITMASK_LENGTH;

                    /*  Prevent unexpected continuation frame. */
                    if (!sws->continuing &&
                        sws->opcode == GRID_WS_OPCODE_FRAGMENT) {
                        grid_sws_fail_conn (sws, GRID_SWS_CLOSE_ERR_PROTO,
                            "No message to continue.");
                        return;
                    }

                    /*  Preserve initial message opcode and RSV bits in case
                        this is a fragmented message. */
                    if (!sws->continuing)
                        sws->inmsg_hdr = sws->inhdr [0] |
                        GRID_SWS_FRAME_BITMASK_FIN;

                    if (sws->payload_ctl <= GRID_SWS_PAYLOAD_MAX_LENGTH) {
                        sws->ext_hdr_len += GRID_SWS_FRAME_SIZE_PAYLOAD_0;
                    }
                    else if (sws->payload_ctl == GRID_SWS_PAYLOAD_FRAME_16) {
                        sws->ext_hdr_len += GRID_SWS_FRAME_SIZE_PAYLOAD_16;
                    }
                    else if (sws->payload_ctl == GRID_SWS_PAYLOAD_FRAME_63) {
                        sws->ext_hdr_len += GRID_SWS_FRAME_SIZE_PAYLOAD_63;
                    }
                    else {
                        /*  Developer error parsing/handling length. */
                        grid_assert (0);
                        return;
                    }

                    switch (sws->opcode) {

                    case GRID_WS_OPCODE_TEXT:
                        /*  Fall thru; TEXT and BINARY handled alike. */
                    case GRID_WS_OPCODE_BINARY:

                        sws->is_control_frame = 0;

                        if (sws->continuing) {
                            grid_sws_fail_conn (sws, GRID_SWS_CLOSE_ERR_PROTO,
                                "Expected continuation frame opcode.");
                            return;
                        }

                        if (!sws->is_final_frame)
                            sws->continuing = 1;

                        if (sws->ext_hdr_len == 0 && sws->payload_ctl == 0) {
                            /*  Only a remote server could send a 2-byte msg;
                                sanity-check that this endpoint is a client. */
                            grid_assert (sws->mode == GRID_WS_CLIENT);

                            sws->inmsg_current_chunk_len = 0;

                            if (sws->continuing) {
                                /*  This frame was empty, but continue
                                    next frame in fragmented sequence. */
                                grid_sws_recv_hdr (sws);
                                return;
                            }
                            else {
                                /*  Special case when there is no payload,
                                    mask, or additional frames. */
                                sws->instate = GRID_SWS_INSTATE_RECVD_CHUNKED;
                                grid_pipebase_received (&sws->pipebase);
                                return;
                            }
                            }
                        /*  Continue to receive extended header+payload. */
                        break;

                    case GRID_WS_OPCODE_FRAGMENT:

                        sws->is_control_frame = 0;
                        sws->continuing = !sws->is_final_frame;

                        if (sws->ext_hdr_len == 0 && sws->payload_ctl == 0) {
                            /*  Only a remote server could send a 2-byte msg;
                                sanity-check that this endpoint is a client. */
                            grid_assert (sws->mode == GRID_WS_CLIENT);

                            sws->inmsg_current_chunk_len = 0;

                            if (sws->continuing) {
                                /*  This frame was empty, but continue
                                    next frame in fragmented sequence. */
                                grid_sws_recv_hdr (sws);
                                return;
                            }
                            else {
                                /*  Special case when there is no payload,
                                    mask, or additional frames. */
                                sws->instate = GRID_SWS_INSTATE_RECVD_CHUNKED;
                                grid_pipebase_received (&sws->pipebase);
                                return;
                            }
                        }
                        /*  Continue to receive extended header+payload. */
                        break;

                    case GRID_WS_OPCODE_PING:
                        sws->is_control_frame = 1;
                        sws->pings_received++;
                        if (sws->payload_ctl > GRID_SWS_PAYLOAD_MAX_LENGTH) {
                            /*  As per RFC 6455 section 5.4, large payloads on
                                control frames is not allowed, and on receipt the
                                endpoint MUST close connection immediately. */
                            grid_sws_fail_conn (sws, GRID_SWS_CLOSE_ERR_PROTO,
                                "Control frame payload exceeds allowable length.");
                            return;
                        }
                        if (!sws->is_final_frame) {
                            /*  As per RFC 6455 section 5.4, fragmentation of
                                control frames is not allowed; on receipt the
                                endpoint MUST close connection immediately. */
                            grid_sws_fail_conn (sws, GRID_SWS_CLOSE_ERR_PROTO,
                                "Cannot fragment control message (FIN=0).");
                            return;
                        }

                        if (sws->ext_hdr_len == 0 && sws->payload_ctl == 0) {
                            /*  Special case when there is no payload,
                                mask, or additional frames. */
                            sws->inmsg_current_chunk_len = 0;
                            sws->instate = GRID_SWS_INSTATE_RECVD_CONTROL;
                            grid_pipebase_received (&sws->pipebase);
                            return;
                        }
                        /*  Continue to receive extended header+payload. */
                        break;
                    
                    case GRID_WS_OPCODE_PONG:
                        sws->is_control_frame = 1;
                        sws->pongs_received++;
                        if (sws->payload_ctl > GRID_SWS_PAYLOAD_MAX_LENGTH) {
                            /*  As per RFC 6455 section 5.4, large payloads on
                                control frames is not allowed, and on receipt the
                                endpoint MUST close connection immediately. */
                            grid_sws_fail_conn (sws, GRID_SWS_CLOSE_ERR_PROTO,
                                "Control frame payload exceeds allowable length.");
                            return;
                        }
                        if (!sws->is_final_frame) {
                            /*  As per RFC 6455 section 5.4, fragmentation of
                                control frames is not allowed; on receipt the
                                endpoint MUST close connection immediately. */
                            grid_sws_fail_conn (sws, GRID_SWS_CLOSE_ERR_PROTO,
                                "Cannot fragment control message (FIN=0).");
                            return;
                        }

                        if (sws->ext_hdr_len == 0 && sws->payload_ctl == 0) {
                            /*  Special case when there is no payload,
                                mask, or additional frames. */
                            sws->inmsg_current_chunk_len = 0;
                            sws->instate = GRID_SWS_INSTATE_RECVD_CONTROL;
                            grid_pipebase_received (&sws->pipebase);
                            return;
                        }
                        /*  Continue to receive extended header+payload. */
                        break;
                    
                    case GRID_WS_OPCODE_CLOSE:
                        /*  RFC 6455 section 5.5.1. */
                        sws->is_control_frame = 1;
                        if (!sws->is_final_frame) {
                            /*  As per RFC 6455 section 5.4, fragmentation of
                                control frames is not allowed; on receipt the
                                endpoint MUST close connection immediately. */
                            grid_sws_fail_conn (sws, GRID_SWS_CLOSE_ERR_PROTO,
                                "Cannot fragment control message (FIN=0).");
                            return;
                        }

                        if (sws->payload_ctl > GRID_SWS_PAYLOAD_MAX_LENGTH) {
                            /*  As per RFC 6455 section 5.4, large payloads on
                                control frames is not allowed, and on receipt the
                                endpoint MUST close connection immediately. */
                            grid_sws_fail_conn (sws, GRID_SWS_CLOSE_ERR_PROTO,
                                "Control frame payload exceeds allowable length.");
                            return;
                        }

                        if (sws->payload_ctl == 1) {
                            /*  As per RFC 6455 section 5.5.1, if a payload is
                                to accompany a close frame, the first two bytes
                                MUST be the close code. */
                            grid_sws_fail_conn (sws, GRID_SWS_CLOSE_ERR_PROTO,
                                "Expected 2byte close code.");
                            return;
                        }

                        if (sws->ext_hdr_len == 0 && sws->payload_ctl == 0) {
                            /*  Special case when there is no payload,
                                mask, or additional frames. */
                            sws->inmsg_current_chunk_len = 0;
                            sws->instate = GRID_SWS_INSTATE_RECVD_CONTROL;
                            grid_pipebase_received (&sws->pipebase);
                            return;
                        }
                        /*  Continue to receive extended header+payload. */
                        break;
                    
                    default:
                        /*  Client sent an invalid opcode; as per RFC 6455
                            section 10.7, close connection with code. */
                        grid_sws_fail_conn (sws, GRID_SWS_CLOSE_ERR_PROTO,
                                "Invalid opcode.");
                        return;

                    }

                    if (sws->ext_hdr_len == 0) {
                        /*  Only a remote server could send a 2-byte msg;
                            sanity-check that this endpoint is a client. */
                        grid_assert (sws->mode == GRID_WS_CLIENT);

                        /*  In the case of no additional header, the payload
                            is known to not exceed this threshold. */
                        grid_assert (sws->payload_ctl <= GRID_SWS_PAYLOAD_MAX_LENGTH);

                        /*  In the case of no additional header, the payload
                            is known to not exceed this threshold. */
                        grid_assert (sws->payload_ctl > 0);

                        sws->instate = GRID_SWS_INSTATE_RECV_PAYLOAD;
                        sws->inmsg_current_chunk_len = sws->payload_ctl;


                        /*  Use scatter/gather array for application messages,
                            and a fixed-width buffer for control messages. This
                            is convenient since control messages can be
                            interspersed between chunked application msgs. */
                        if (sws->is_control_frame) {
                            sws->inmsg_current_chunk_buf = sws->inmsg_control;
                        }
                        else {
                            sws->inmsg_chunks++;
                            sws->inmsg_total_size += sws->inmsg_current_chunk_len;
                            sws->inmsg_current_chunk_buf =
                                grid_msg_chunk_new (sws->inmsg_current_chunk_len,
                                &sws->inmsg_array);
                        }

                        grid_usock_recv (sws->usock, sws->inmsg_current_chunk_buf,
                            sws->inmsg_current_chunk_len, NULL);
                        return;
                    }
                    else {
                        /*  Continue receiving the rest of the header frame. */
                        sws->instate = GRID_SWS_INSTATE_RECV_HDREXT;
                        grid_usock_recv (sws->usock,
                            sws->inhdr + GRID_SWS_FRAME_SIZE_INITIAL,
                            sws->ext_hdr_len,
                            NULL);
                        return;
                    }

                case GRID_SWS_INSTATE_RECV_HDREXT:
                    grid_assert (sws->ext_hdr_len > 0);

                    if (sws->payload_ctl <= GRID_SWS_PAYLOAD_MAX_LENGTH) {
                        sws->inmsg_current_chunk_len = sws->payload_ctl;
                        if (sws->masked) {
                            sws->mask = sws->inhdr + GRID_SWS_FRAME_SIZE_INITIAL;
                        }
                        else {
                            sws->mask = NULL;
                        }
                    }
                    else if (sws->payload_ctl == GRID_SWS_PAYLOAD_FRAME_16) {
                        sws->inmsg_current_chunk_len =
                            grid_gets (sws->inhdr + GRID_SWS_FRAME_SIZE_INITIAL);
                        if (sws->masked) {
                            sws->mask = sws->inhdr +
                                GRID_SWS_FRAME_SIZE_INITIAL +
                                GRID_SWS_FRAME_SIZE_PAYLOAD_16;
                        }
                        else {
                            sws->mask = NULL;
                        }
                    }
                    else if (sws->payload_ctl == GRID_SWS_PAYLOAD_FRAME_63) {
                        sws->inmsg_current_chunk_len =
                            (size_t) grid_getll (sws->inhdr +
                            GRID_SWS_FRAME_SIZE_INITIAL);
                        if (sws->masked) {
                            sws->mask = sws->inhdr +
                                GRID_SWS_FRAME_SIZE_INITIAL +
                                GRID_SWS_FRAME_SIZE_PAYLOAD_63;
                        }
                        else {
                            sws->mask = NULL;
                        }
                    }
                    else {
                        /*  Client sent invalid data; as per RFC 6455,
                            server closes the connection immediately. */
                        grid_sws_fail_conn (sws, GRID_SWS_CLOSE_ERR_PROTO,
                                "Invalid payload length.");
                        return;
                    }

                    /*  Handle zero-length message bodies. */
                    if (sws->inmsg_current_chunk_len == 0)
                    {
                        if (sws->is_final_frame) {
                           if (sws->opcode == GRID_WS_OPCODE_CLOSE) {
                             grid_pipebase_stop (&sws->pipebase);
                             sws->state = GRID_SWS_STATE_CLOSING_CONNECTION;
                            }
                            else
                            { 
                              sws->instate = (sws->is_control_frame ?
                                  GRID_SWS_INSTATE_RECVD_CONTROL :
                                  GRID_SWS_INSTATE_RECVD_CHUNKED);
                              grid_pipebase_received (&sws->pipebase);
                            }
                        }
                        else {
                            grid_sws_recv_hdr (sws);
                        }
			return;
                    }

                    grid_assert (sws->inmsg_current_chunk_len > 0);

                    /*  Use scatter/gather array for application messages,
                        and a fixed-width buffer for control messages. This
                        is convenient since control messages can be
                        interspersed between chunked application msgs. */
                    if (sws->is_control_frame) {
                        sws->inmsg_current_chunk_buf = sws->inmsg_control;
                    }
                    else {
                        sws->inmsg_chunks++;
                        sws->inmsg_total_size += sws->inmsg_current_chunk_len;
                        sws->inmsg_current_chunk_buf =
                            grid_msg_chunk_new (sws->inmsg_current_chunk_len,
                            &sws->inmsg_array);
                    }

                    sws->instate = GRID_SWS_INSTATE_RECV_PAYLOAD;
                    grid_usock_recv (sws->usock, sws->inmsg_current_chunk_buf,
                        sws->inmsg_current_chunk_len, NULL);
                    return;

                case GRID_SWS_INSTATE_RECV_PAYLOAD:

                    /*  Unmask if necessary. */
                    if (sws->masked) {
                        grid_sws_mask_payload (sws->inmsg_current_chunk_buf,
                            sws->inmsg_current_chunk_len, sws->mask,
                            GRID_SWS_FRAME_SIZE_MASK, NULL);
                    }

                    switch (sws->opcode) {

                    case GRID_WS_OPCODE_TEXT:
                        grid_sws_validate_utf8_chunk (sws);
                        return;

                    case GRID_WS_OPCODE_BINARY:
                        if (sws->is_final_frame) {
                            sws->instate = GRID_SWS_INSTATE_RECVD_CHUNKED;
                            grid_pipebase_received (&sws->pipebase);
                        }
                        else {
                            grid_sws_recv_hdr (sws);
                        }
                        return;

                    case GRID_WS_OPCODE_FRAGMENT:
                        /*  Must check original opcode to see if this fragment
                            needs UTF-8 validation. */
                        if ((sws->inmsg_hdr & GRID_SWS_FRAME_BITMASK_OPCODE) ==
                            GRID_WS_OPCODE_TEXT) {
                            grid_sws_validate_utf8_chunk (sws);
                        }
                        else if (sws->is_final_frame) {
                            sws->instate = GRID_SWS_INSTATE_RECVD_CHUNKED;
                            grid_pipebase_received (&sws->pipebase);
                        }
                        else {
                            grid_sws_recv_hdr (sws);
                        }
                        return;

                    case GRID_WS_OPCODE_PING:
                        sws->instate = GRID_SWS_INSTATE_RECVD_CONTROL;
                        grid_pipebase_received (&sws->pipebase);
                        return;

                    case GRID_WS_OPCODE_PONG:
                        sws->instate = GRID_SWS_INSTATE_RECVD_CONTROL;
                        grid_pipebase_received (&sws->pipebase);
                        return;

                    case GRID_WS_OPCODE_CLOSE:
                        /*  If the payload is not even long enough for the
                            required 2-octet Close Code, the connection
                            should have been failed upstream. */
                        grid_assert (sws->inmsg_current_chunk_len >=
                            GRID_SWS_CLOSE_CODE_LEN);
                        
                        grid_pipebase_stop (&sws->pipebase);
                        sws->state = GRID_SWS_STATE_CLOSING_CONNECTION;
                        return;

                    default:
                        /*  This should have been prevented upstream. */
                        grid_assert (0);
                        return;
                    } 

                default:
                    grid_fsm_error ("Unexpected socket instate",
                        sws->state, src, type);
                }

            case GRID_USOCK_SHUTDOWN:
                grid_pipebase_stop (&sws->pipebase);
                sws->state = GRID_SWS_STATE_BROKEN_CONNECTION;
                return;

            case GRID_USOCK_ERROR:
                grid_pipebase_stop (&sws->pipebase);
                sws->state = GRID_SWS_STATE_DONE;
                grid_fsm_raise (&sws->fsm, &sws->done, GRID_SWS_RETURN_ERROR);
                return;

            default:
                grid_fsm_bad_action (sws->state, src, type);
            }

            break;

        default:
            grid_fsm_bad_source (sws->state, src, type);
        }

/******************************************************************************/
/*  CLOSING_CONNECTION state.                                                 */
/*  Wait for acknowledgement closing handshake was successfully sent.         */
/******************************************************************************/
    case GRID_SWS_STATE_CLOSING_CONNECTION:
        switch (src) {

        case GRID_SWS_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SENT:
                /*  Wait for acknowledgement closing handshake was sent
                    to peer. */
                grid_assert (sws->outstate == GRID_SWS_OUTSTATE_SENDING);
                sws->outstate = GRID_SWS_OUTSTATE_IDLE;
                sws->state = GRID_SWS_STATE_DONE;
                grid_fsm_raise (&sws->fsm, &sws->done,
                    GRID_SWS_RETURN_CLOSE_HANDSHAKE);
                return;
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_USOCK_ERROR:
                sws->state = GRID_SWS_STATE_DONE;
                grid_fsm_raise (&sws->fsm, &sws->done, GRID_SWS_RETURN_ERROR);
                return;
            default:
                grid_fsm_bad_action (sws->state, src, type);
            }

        default:
            grid_fsm_bad_source (sws->state, src, type);
        }

/******************************************************************************/
/*  SHUTTING_DOWN state.                                                      */
/*  The underlying connection is closed. We are just waiting that underlying  */
/*  usock being closed                                                        */
/******************************************************************************/
    case GRID_SWS_STATE_BROKEN_CONNECTION:
        switch (src) {

        case GRID_SWS_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_ERROR:
                sws->state = GRID_SWS_STATE_DONE;
                grid_fsm_raise (&sws->fsm, &sws->done, GRID_SWS_RETURN_ERROR);
                return;
            default:
                grid_fsm_bad_action (sws->state, src, type);
            }

        default:
            grid_fsm_bad_source (sws->state, src, type);
        }

/******************************************************************************/
/*  DONE state.                                                               */
/*  The underlying connection is closed. There's nothing that can be done in  */
/*  this state except stopping the object.                                    */
/******************************************************************************/
    case GRID_SWS_STATE_DONE:
        grid_fsm_bad_source (sws->state, src, type);

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (sws->state, src, type);
    }
}

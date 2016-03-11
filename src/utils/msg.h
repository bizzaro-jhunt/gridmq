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

#ifndef GRID_MSG_INCLUDED
#define GRID_MSG_INCLUDED

#include "chunkref.h"

#include <stddef.h>

struct grid_msg {

    /*  Contains SP message header. This field directly corresponds
        to SP message header as defined in SP RFCs. There's no leading
        cmsghdr or trailing padding. */
    struct grid_chunkref sphdr;

    /*  Contains any additional transport-level message headers. Format of this
        buffer is a list of cmsgs as defined by POSIX (see "ancillary data"). */
    struct grid_chunkref hdrs;

    /*  Contains application level message payload. */
    struct grid_chunkref body;
};

/*  Initialises a message with body 'size' bytes long and empty header. */
void grid_msg_init (struct grid_msg *self, size_t size);

/*  Initialise message with body provided in the form of chunk pointer. */
void grid_msg_init_chunk (struct grid_msg *self, void *chunk);

/*  Frees resources allocate with the message. */
void grid_msg_term (struct grid_msg *self);

/*  Moves the content of the message from src to dst. dst should not be
    initialised prior to the operation. src will be uninitialised after the
    operation. */
void grid_msg_mv (struct grid_msg *dst, struct grid_msg *src);

/*  Copies a message from src to dst. dst should not be
    initialised prior to the operation. */
void grid_msg_cp (struct grid_msg *dst, struct grid_msg *src);

/*  Bulk copying is done by first invoking grid_msg_bulkcopy_start on the source
    message and specifying how many copies of the message will be made. Then,
    grid_msg_bulkcopy_cp should be used 'copies' of times to make individual
    copies of the source message. Note: Bulk copying is more efficient than
    making each copy separately. */
void grid_msg_bulkcopy_start (struct grid_msg *self, uint32_t copies);
void grid_msg_bulkcopy_cp (struct grid_msg *dst, struct grid_msg *src);

/** Replaces the message body with entirely new data.  This allows protocols
    that substantially rewrite or preprocess the userland message to be written. */
void grid_msg_replace_body(struct grid_msg *self, struct grid_chunkref newBody);

#endif


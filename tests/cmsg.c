/*
    Copyright (c) 2014 Martin Sustrik  All rights reserved.
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

#include "../src/grid.h"
#include "../src/tcp.h"
#include "../src/reqrep.h"

#include "testutil.h"

#define SOCKET_ADDRESS "tcp://127.0.0.1:5555"

int main ()
{
    int rc;
    int rep;
    int req;
    struct grid_msghdr hdr;
    struct grid_iovec iovec;
    unsigned char body [3];
    unsigned char ctrl [256];
    struct grid_cmsghdr *cmsg;
    unsigned char *data;
    void *buf;
    
    rep = test_socket (AF_SP_RAW, GRID_REP);
    test_bind (rep, SOCKET_ADDRESS);
    req = test_socket (AF_SP, GRID_REQ);
    test_connect (req, SOCKET_ADDRESS);

    /* Test ancillary data in static buffer. */

    test_send (req, "ABC");

    iovec.iov_base = body;
    iovec.iov_len = sizeof (body);
    hdr.msg_iov = &iovec;
    hdr.msg_iovlen = 1;
    hdr.msg_control = ctrl;
    hdr.msg_controllen = sizeof (ctrl);
    rc = grid_recvmsg (rep, &hdr, 0);
    errno_assert (rc == 3);

    cmsg = GRID_CMSG_FIRSTHDR (&hdr);
    while (1) {
        grid_assert (cmsg);
        if (cmsg->cmsg_level == PROTO_SP && cmsg->cmsg_type == SP_HDR)
            break;
        cmsg = GRID_CMSG_NXTHDR (&hdr, cmsg);
    }
    grid_assert (cmsg->cmsg_len == GRID_CMSG_SPACE (8+sizeof (size_t)));
    data = GRID_CMSG_DATA (cmsg);
    grid_assert (!(data[0+sizeof (size_t)] & 0x80));
    grid_assert (data[4+sizeof (size_t)] & 0x80);

    rc = grid_sendmsg (rep, &hdr, 0);
    grid_assert (rc == 3);
    test_recv (req, "ABC");

    /* Test ancillary data in dynamically allocated buffer (GRID_MSG). */

    test_send (req, "ABC");

    iovec.iov_base = body;
    iovec.iov_len = sizeof (body);
    hdr.msg_iov = &iovec;
    hdr.msg_iovlen = 1;
    hdr.msg_control = &buf;
    hdr.msg_controllen = GRID_MSG;
    rc = grid_recvmsg (rep, &hdr, 0);
    errno_assert (rc == 3);

    cmsg = GRID_CMSG_FIRSTHDR (&hdr);
    while (1) {
        grid_assert (cmsg);
        if (cmsg->cmsg_level == PROTO_SP && cmsg->cmsg_type == SP_HDR)
            break;
        cmsg = GRID_CMSG_NXTHDR (&hdr, cmsg);
    }
    grid_assert (cmsg->cmsg_len == GRID_CMSG_SPACE (8+sizeof (size_t)));
    data = GRID_CMSG_DATA (cmsg);
    grid_assert (!(data[0+sizeof (size_t)] & 0x80));
    grid_assert (data[4+sizeof (size_t)] & 0x80);

    rc = grid_sendmsg (rep, &hdr, 0);
    grid_assert (rc == 3);
    test_recv (req, "ABC");

    test_close (req);
    test_close (rep);

    return 0;
}


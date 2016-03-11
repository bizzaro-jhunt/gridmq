/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.

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
#include "../src/pair.h"

#include "testutil.h"

#include <string.h>

#define SOCKET_ADDRESS "inproc://a"
#define SOCKET_ADDRESS_TCP "tcp://127.0.0.1:5557"

char longdata[1 << 20];

int main ()
{
    int rc;
    int sb;
    int sc;
    unsigned char *buf1, *buf2;
    int i;
    struct grid_iovec iov;
    struct grid_msghdr hdr;

    sb = test_socket (AF_SP, GRID_PAIR);
    test_bind (sb, SOCKET_ADDRESS);
    sc = test_socket (AF_SP, GRID_PAIR);
    test_connect (sc, SOCKET_ADDRESS);

    buf1 = grid_allocmsg (256, 0);
    alloc_assert (buf1);
    for (i = 0; i != 256; ++i)
        buf1 [i] = (unsigned char) i;
    rc = grid_send (sc, &buf1, GRID_MSG, 0);
    errno_assert (rc >= 0);
    grid_assert (rc == 256);

    buf2 = NULL;
    rc = grid_recv (sb, &buf2, GRID_MSG, 0);
    errno_assert (rc >= 0);
    grid_assert (rc == 256);
    grid_assert (buf2);
    for (i = 0; i != 256; ++i)
        grid_assert (buf2 [i] == (unsigned char) i);
    rc = grid_freemsg (buf2);
    errno_assert (rc == 0);

    buf1 = grid_allocmsg (256, 0);
    alloc_assert (buf1);
    for (i = 0; i != 256; ++i)
        buf1 [i] = (unsigned char) i;
    iov.iov_base = &buf1;
    iov.iov_len = GRID_MSG;
    memset (&hdr, 0, sizeof (hdr));
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    rc = grid_sendmsg (sc, &hdr, 0);
    errno_assert (rc >= 0);
    grid_assert (rc == 256);

    buf2 = NULL;
    iov.iov_base = &buf2;
    iov.iov_len = GRID_MSG;
    memset (&hdr, 0, sizeof (hdr));
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    rc = grid_recvmsg (sb, &hdr, 0);
    errno_assert (rc >= 0);
    grid_assert (rc == 256);
    grid_assert (buf2);
    for (i = 0; i != 256; ++i)
        grid_assert (buf2 [i] == (unsigned char) i);
    rc = grid_freemsg (buf2);
    errno_assert (rc == 0);

    test_close (sc);
    test_close (sb);

    /*  Test receiving of large message  */

    sb = test_socket (AF_SP, GRID_PAIR);
    test_bind (sb, SOCKET_ADDRESS_TCP);
    sc = test_socket (AF_SP, GRID_PAIR);
    test_connect (sc, SOCKET_ADDRESS_TCP);

    for (i = 0; i < (int) sizeof (longdata); ++i)
        longdata[i] = '0' + (i % 10);
    longdata [sizeof (longdata) - 1] = 0;
    test_send (sb, longdata);

    rc = grid_recv (sc, &buf2, GRID_MSG, 0);
    errno_assert (rc >= 0);
    grid_assert (rc == sizeof (longdata) - 1);
    grid_assert (buf2);
    for (i = 0; i < (int) sizeof (longdata) - 1; ++i)
        grid_assert (buf2 [i] == longdata [i]);
    rc = grid_freemsg (buf2);
    errno_assert (rc == 0);

    test_close (sc);
    test_close (sb);

    return 0;
}


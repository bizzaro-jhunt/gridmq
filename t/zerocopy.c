/*
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.
    Copyright (c) 2014 Achille Roussel. All rights reserved.

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
#include "../src/pubsub.h"
#include "../src/reqrep.h"

#include "testutil.h"

/*
 * Nanomsg never zero copies anymore - it used to be an attribute of
 * the inproc transport, but frankly its a mistake for anyone to depend
 * on that.  The implementation must be free to copy, move data, etc.
 * The only thing that should be guaranteed is that the "ownership" of the
 * message on send is passed to libgridmq.  libgridmq may give that message
 * to an inproc receiver, or it can do something else (like copy the data)
 * with it.
 */
#if 0

#include <string.h>

void test_allocmsg_reqrep ()
{
    int rc;
    int req;
    void *p;
    struct grid_iovec iov;
    struct grid_msghdr hdr;

    /*  Try to create an oversized message. */
    p = grid_allocmsg (-1, 0);
    grid_assert (!p && grid_errno () == ENOMEM);
    p = grid_allocmsg (-3, 0);
    grid_assert (!p && grid_errno () == ENOMEM);

    /*  Try to create a message of unknown type. */
    p = grid_allocmsg (100, 333);
    grid_assert (!p && grid_errno () == EINVAL);

    /*  Create a socket. */
    req = test_socket (AF_SP_RAW, GRID_REQ);

    /*  Make send fail and check whether the zero-copy buffer is left alone
        rather than deallocated. */
    p = grid_allocmsg (100, 0);
    grid_assert (p);
    rc = grid_send (req, &p, GRID_MSG, GRID_DONTWAIT);
    grid_assert (rc < 0);
    errno_assert (grid_errno () == EAGAIN);
    memset (p, 0, 100);
    rc = grid_freemsg (p);
    errno_assert (rc == 0);

    /*  Same thing with grid_sendmsg(). */
    p = grid_allocmsg (100, 0);
    grid_assert (p);
    iov.iov_base = &p;
    iov.iov_len = GRID_MSG;
    memset (&hdr, 0, sizeof (hdr));
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    grid_sendmsg (req, &hdr, GRID_DONTWAIT);
    errno_assert (grid_errno () == EAGAIN);
    memset (p, 0, 100);
    rc = grid_freemsg (p);
    errno_assert (rc == 0);

    /*  Clean up. */
    test_close (req);
}

void test_reallocmsg_reqrep ()
{
    int rc;
    int req;
    int rep;
    void *p;
    void *p2;

    /*  Create sockets. */
    req = grid_socket (AF_SP, GRID_REQ);
    rep = grid_socket (AF_SP, GRID_REP);
    rc = grid_bind (rep, "inproc://test");
    errno_assert (rc >= 0);
    rc = grid_connect (req, "inproc://test");
    errno_assert (rc >= 0);

    /*  Create message, make sure we handle overflow. */
    p = grid_allocmsg (100, 0);
    grid_assert (p);
    p2 = grid_reallocmsg (p, (size_t)-3);
    errno_assert (grid_errno () == ENOMEM);
    grid_assert (p2 == NULL);

    /*  Realloc to fit data size. */
    memcpy (p, "Hello World!", 12);
    p = grid_reallocmsg (p, 12);
    grid_assert (p);
    rc = grid_send (req, &p, GRID_MSG, 0);
    errno_assert (rc == 12);

    /*  Receive request and send response. */
    rc = grid_recv (rep, &p, GRID_MSG, 0);
    errno_assert (rc == 12);
    rc = grid_send (rep, &p, GRID_MSG, 0);
    errno_assert (rc == 12);

    /*  Receive response and free message. */
    rc = grid_recv (req, &p, GRID_MSG, 0);
    errno_assert (rc == 12);
    rc = memcmp (p, "Hello World!", 12);
    grid_assert (rc == 0);
    rc = grid_freemsg (p);
    errno_assert (rc == 0);

    /*  Clean up. */
    grid_close (req);
    grid_close (rep);
}

void test_reallocmsg_pubsub ()
{
    int rc;
    int pub;
    int sub1;
    int sub2;
    void *p;
    void *p1;
    void *p2;

    /*  Create sockets. */
    pub = grid_socket (AF_SP, GRID_PUB);
    sub1 = grid_socket (AF_SP, GRID_SUB);
    sub2 = grid_socket (AF_SP, GRID_SUB);
    rc = grid_bind (pub, "inproc://test");
    errno_assert (rc >= 0);
    rc = grid_connect (sub1, "inproc://test");
    errno_assert (rc >= 0);
    rc = grid_connect (sub2, "inproc://test");
    errno_assert (rc >= 0);
    rc = grid_setsockopt (sub1, GRID_SUB, GRID_SUB_SUBSCRIBE, "", 0);
    errno_assert (rc == 0);
    rc = grid_setsockopt (sub2, GRID_SUB, GRID_SUB_SUBSCRIBE, "", 0);
    errno_assert (rc == 0);

    /*  Publish message. */
    p = grid_allocmsg (12, 0);
    grid_assert (p);
    memcpy (p, "Hello World!", 12);
    rc = grid_send (pub, &p, GRID_MSG, 0);
    errno_assert (rc == 12);

    /*  Receive messages, both messages are the same object with inproc. */
    rc = grid_recv (sub1, &p1, GRID_MSG, 0);
    errno_assert (rc == 12);
    rc = grid_recv (sub2, &p2, GRID_MSG, 0);
    errno_assert (rc == 12);
    grid_assert (p1 == p2);
    rc = memcmp (p1, "Hello World!", 12);
    grid_assert (rc == 0);
    rc = memcmp (p2, "Hello World!", 12);
    grid_assert (rc == 0);

    /*  Reallocate one message, both messages shouldn't be the same object
        anymore. */
    p1 = grid_reallocmsg (p1, 15);
    errno_assert (p1);
    grid_assert (p1 != p2);
    memcpy (((char*) p1) + 12, " 42", 3);
    rc = memcmp (p1, "Hello World! 42", 15);
    grid_assert (rc == 0);

    /*  Release messages. */
    rc = grid_freemsg (p1);
    errno_assert (rc == 0);
    rc = grid_freemsg (p2);
    errno_assert (rc == 0);

    /*  Clean up. */
    grid_close (sub2);
    grid_close (sub1);
    grid_close (pub);
}
#endif

int main ()
{
#if 0
    test_allocmsg_reqrep ();
    test_reallocmsg_reqrep ();
    test_reallocmsg_pubsub ();
#endif
    return 0;
}


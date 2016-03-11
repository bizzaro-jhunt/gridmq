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
#include "../src/inproc.h"

#include "testutil.h"
#include "../src/utils/attr.h"
#include "../src/utils/thread.c"

#include <sys/select.h>

/*  Test of polling via GRID_SNDFD/GRID_RCVFD mechanism. */

#define SOCKET_ADDRESS "inproc://a"

int sc;

void routine1 (GRID_UNUSED void *arg)
{
   grid_sleep (10);
   test_send (sc, "ABC");
}

void routine2 (GRID_UNUSED void *arg)
{
   grid_sleep (10);
   grid_term ();
}

#define GRID_IN 1
#define GRID_OUT 2

int getevents (int s, int events, int timeout)
{
    int rc;
    fd_set pollset;
    int rcvfd;
    int sndfd;
    int maxfd;
    size_t fdsz;
    struct timeval tv;
    int revents;

    maxfd = 0;
    FD_ZERO (&pollset);

    if (events & GRID_IN) {
        fdsz = sizeof (rcvfd);
        rc = grid_getsockopt (s, GRID_SOL_SOCKET, GRID_RCVFD, (char*) &rcvfd, &fdsz);
        errno_assert (rc == 0);
        grid_assert (fdsz == sizeof (rcvfd));
        FD_SET (rcvfd, &pollset);
        if (rcvfd + 1 > maxfd)
            maxfd = rcvfd + 1;
    }

    if (events & GRID_OUT) {
        fdsz = sizeof (sndfd);
        rc = grid_getsockopt (s, GRID_SOL_SOCKET, GRID_SNDFD, (char*) &sndfd, &fdsz);
        errno_assert (rc == 0);
        grid_assert (fdsz == sizeof (sndfd));
        FD_SET (sndfd, &pollset);
        if (sndfd + 1 > maxfd)
            maxfd = sndfd + 1;
    }

    if (timeout >= 0) {
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
    }
    rc = select (maxfd, &pollset, NULL, NULL, timeout < 0 ? NULL : &tv);
    errno_assert (rc >= 0);

    revents = 0;
    if ((events & GRID_IN) && FD_ISSET (rcvfd, &pollset))
        revents |= GRID_IN;
    if ((events & GRID_OUT) && FD_ISSET (sndfd, &pollset))
        revents |= GRID_OUT;
    return revents;
}

int main ()
{
    int rc;
    int sb;
    char buf [3];
    struct grid_thread thread;
    struct grid_pollfd pfd [2];

    /* Test grid_poll() function. */
    sb = test_socket (AF_SP, GRID_PAIR);
    test_bind (sb, SOCKET_ADDRESS);
    sc = test_socket (AF_SP, GRID_PAIR);
    test_connect (sc, SOCKET_ADDRESS);
    test_send (sc, "ABC");
    grid_sleep (100);
    pfd [0].fd = sb;
    pfd [0].events = GRID_POLLIN | GRID_POLLOUT;
    pfd [1].fd = sc;
    pfd [1].events = GRID_POLLIN | GRID_POLLOUT;
    rc = grid_poll (pfd, 2, -1);
    errno_assert (rc >= 0);
    grid_assert (rc == 2);
    grid_assert (pfd [0].revents == (GRID_POLLIN | GRID_POLLOUT));
    grid_assert (pfd [1].revents == GRID_POLLOUT);
    test_close (sc);
    test_close (sb);

    /*  Create a simple topology. */
    sb = test_socket (AF_SP, GRID_PAIR);
    test_bind (sb, SOCKET_ADDRESS);
    sc = test_socket (AF_SP, GRID_PAIR);
    test_connect (sc, SOCKET_ADDRESS);

    /*  Check the initial state of the socket. */
    rc = getevents (sb, GRID_IN | GRID_OUT, 1000);
    grid_assert (rc == GRID_OUT);

    /*  Poll for IN when there's no message available. The call should
        time out. */
    rc = getevents (sb, GRID_IN, 10);
    grid_assert (rc == 0);

    /*  Send a message and start polling. This time IN event should be
        signaled. */
    test_send (sc, "ABC");
    rc = getevents (sb, GRID_IN, 1000);
    grid_assert (rc == GRID_IN);

    /*  Receive the message and make sure that IN is no longer signaled. */
    test_recv (sb, "ABC");
    rc = getevents (sb, GRID_IN, 10);
    grid_assert (rc == 0);

    /*  Check signalling from a different thread. */
    grid_thread_init (&thread, routine1, NULL);
    rc = getevents (sb, GRID_IN, 1000);
    grid_assert (rc == GRID_IN);
    test_recv (sb, "ABC");
    grid_thread_term (&thread);

    /*  Check terminating the library from a different thread. */
    grid_thread_init (&thread, routine2, NULL);
    rc = getevents (sb, GRID_IN, 1000);
    grid_assert (rc == GRID_IN);
    rc = grid_recv (sb, buf, sizeof (buf), 0);
    grid_assert (rc < 0 && grid_errno () == ETERM);
    grid_thread_term (&thread);

    /*  Clean up. */
    test_close (sc);
    test_close (sb);

    return 0;
}


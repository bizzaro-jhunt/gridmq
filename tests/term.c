/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.

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

#include "../src/utils/thread.c"
#include "testutil.h"

static void worker (GRID_UNUSED void *arg)
{
    int rc;
    int s;
    char buf [3];

    /*  Test socket. */
    s = test_socket (AF_SP, GRID_PAIR);

    /*  Launch blocking function to check that it will be unblocked once
        grid_term() is called from the main thread. */
    rc = grid_recv (s, buf, sizeof (buf), 0);
    grid_assert (rc == -1 && grid_errno () == ETERM);

    /*  Check that all subsequent operations fail in synchronous manner. */
    rc = grid_recv (s, buf, sizeof (buf), 0);
    grid_assert (rc == -1 && grid_errno () == ETERM);
    test_close (s);
}

int main ()
{
    int rc;
    int s;
    struct grid_thread thread;

    /*  Close the socket with no associated endpoints. */
    s = test_socket (AF_SP, GRID_PAIR);
    test_close (s);

    /*  Test grid_term() before grid_close(). */
    grid_thread_init (&thread, worker, NULL);
    grid_sleep (100);
    grid_term ();

    /*  Check that it's not possible to create new sockets after grid_term(). */
    rc = grid_socket (AF_SP, GRID_PAIR);
    grid_assert (rc == -1);
    errno_assert (grid_errno () == ETERM);

    /*  Wait till worker thread terminates. */
    grid_thread_term (&thread);

    return 0;
}


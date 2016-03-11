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

#include "../src/utils/attr.h"

#include "../src/utils/err.c"
#include "../src/utils/thread.c"
#include "../src/utils/sleep.c"
#include "../src/utils/stopwatch.c"

#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static size_t message_size;
static int roundtrip_count;

void worker (GRID_UNUSED void *arg)
{
    int rc;
    int s;
    int i;
    char *buf;

    s = grid_socket (AF_SP, GRID_PAIR);
    assert (s != -1);
    rc = grid_connect (s, "inproc://inproc_lat");
    assert (rc >= 0);

    buf = malloc (message_size);
    assert (buf);

    for (i = 0; i != roundtrip_count; i++) {
        rc = grid_recv (s, buf, message_size, 0);
        assert (rc == (int)message_size);
        rc = grid_send (s, buf, message_size, 0);
        assert (rc == (int)message_size);
    }

    free (buf);
    rc = grid_close (s);
    assert (rc == 0);
}

int main (int argc, char *argv [])
{
    int rc;
    int s;
    int i;
    char *buf;
    struct grid_thread thread;
    struct grid_stopwatch stopwatch;
    uint64_t elapsed;
    double latency;

    if (argc != 3) {
        printf ("usage: inproc_lat <message-size> <roundtrip-count>\n");
        return 1;
    }

    message_size = atoi (argv [1]);
    roundtrip_count = atoi (argv [2]);

    s = grid_socket (AF_SP, GRID_PAIR);
    assert (s != -1);
    rc = grid_bind (s, "inproc://inproc_lat");
    assert (rc >= 0);

    buf = malloc (message_size);
    assert (buf);
    memset (buf, 111, message_size);

    /*  Wait a bit till the worker thread blocks in grid_recv(). */
    grid_thread_init (&thread, worker, NULL);
    grid_sleep (100);

    grid_stopwatch_init (&stopwatch);

    for (i = 0; i != roundtrip_count; i++) {
        rc = grid_send (s, buf, message_size, 0);
        assert (rc == (int)message_size);
        rc = grid_recv (s, buf, message_size, 0);
        assert (rc == (int)message_size);
    }

    elapsed = grid_stopwatch_term (&stopwatch);

    latency = (double) elapsed / (roundtrip_count * 2);
    printf ("message size: %d [B]\n", (int) message_size);
    printf ("roundtrip count: %d\n", (int) roundtrip_count);
    printf ("average latency: %.3f [us]\n", (double) latency);

    grid_thread_term (&thread);
    free (buf);
    rc = grid_close (s);
    assert (rc == 0);

    return 0;
}


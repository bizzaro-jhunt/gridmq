/*
    Copyright (c) 2012 Martin Sustrik  All rights reserved.
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
#include "../src/pair.h"
#include "../src/pubsub.h"
#include "../src/pipeline.h"
#include "../src/ipc.h"

#include "testutil.h"
#include "../src/utils/thread.c"
#include "../src/utils/atomic.h"
#include "../src/utils/atomic.c"

/*  Stress test the IPC transport. */

#define THREAD_COUNT 10
#define TEST_LOOPS 10
#define SOCKET_ADDRESS "ipc://test-stress.ipc"

static void server(GRID_UNUSED void *arg)
{
    int bytes;
    int count;
    int sock = grid_socket(AF_SP, GRID_PULL);
    int res[TEST_LOOPS];
    grid_assert(sock >= 0);
    grid_assert(grid_bind(sock, SOCKET_ADDRESS) >= 0);
    count = THREAD_COUNT * TEST_LOOPS;
    memset(res, 0, sizeof (res));
    while (count > 0)
    {
        char *buf = NULL;
        int tid;
        int num;
        bytes = grid_recv(sock, &buf, GRID_MSG, 0);
        grid_assert(bytes >= 0);
        grid_assert(bytes >= 2);
        grid_assert(buf[0] >= 'A' && buf[0] <= 'Z');
        grid_assert(buf[1] >= 'a' && buf[0] <= 'z');
        tid = buf[0]-'A';
        num = buf[1]-'a';
        grid_assert(tid < THREAD_COUNT);
        grid_assert(res[tid] == num);
        res[tid]=num+1;
        grid_freemsg(buf);
        count--;
    }
    grid_close(sock);
}

static void client(void *arg)
{
    intptr_t id = (intptr_t)arg;
    int bytes;
    char msg[3];
    int sz_msg;
    int i;

    msg[0] = 'A' + id%26;
    msg[1] = 'a';
    msg[2] = '\0';
    sz_msg = strlen (msg) + 1; // '\0' too

    for (i = 0; i < TEST_LOOPS; i++) {
        int cli_sock = grid_socket(AF_SP, GRID_PUSH);
        msg[1] = 'a' + i%26;
        grid_assert(cli_sock >= 0);
        grid_assert(grid_connect(cli_sock, SOCKET_ADDRESS) >= 0);
        /*  Give time to allow for connect to establish. */
        grid_sleep(50);
        bytes = grid_send(cli_sock, msg, sz_msg, 0);
        /*  This would better be handled via semaphore or condvar. */
        grid_sleep(100);
        grid_assert(bytes == sz_msg);
        grid_close(cli_sock);
    }
}

int main()
{
    int i;
    struct grid_thread srv_thread;
    struct grid_thread cli_threads[THREAD_COUNT];
    /*  Stress the shutdown algorithm. */
    grid_thread_init(&srv_thread, server, NULL);

    for (i = 0; i != THREAD_COUNT; ++i)
        grid_thread_init(&cli_threads[i], client, (void *)(intptr_t)i);
    for (i = 0; i != THREAD_COUNT; ++i)
        grid_thread_term(&cli_threads[i]);

    return 0;
}

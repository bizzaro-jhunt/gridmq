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

#ifndef GRID_POLLER_INCLUDED
#define GRID_POLLER_INCLUDED

#define GRID_POLLER_IN 1
#define GRID_POLLER_OUT 2
#define GRID_POLLER_ERR 3

#if defined GRID_USE_POLL
#include "poller_poll.h"
#elif defined GRID_USE_EPOLL
#include "poller_epoll.h"
#elif defined GRID_USE_KQUEUE
#include "poller_kqueue.h"
#endif

int grid_poller_init (struct grid_poller *self);
void grid_poller_term (struct grid_poller *self);
void grid_poller_add (struct grid_poller *self, int fd,
    struct grid_poller_hndl *hndl);
void grid_poller_rm (struct grid_poller *self, struct grid_poller_hndl *hndl);
void grid_poller_set_in (struct grid_poller *self, struct grid_poller_hndl *hndl);
void grid_poller_reset_in (struct grid_poller *self, struct grid_poller_hndl *hndl);
void grid_poller_set_out (struct grid_poller *self, struct grid_poller_hndl *hndl);
void grid_poller_reset_out (struct grid_poller *self, struct grid_poller_hndl *hndl);
int grid_poller_wait (struct grid_poller *self, int timeout);
int grid_poller_event (struct grid_poller *self, int *event,
    struct grid_poller_hndl **hndl);


#endif


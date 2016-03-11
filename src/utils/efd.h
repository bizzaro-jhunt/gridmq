/*
    Copyright (c) 2012-2013 Martin Sustrik  All rights reserved.
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

#ifndef GRID_EFD_INCLUDED
#define GRID_EFD_INCLUDED

/*  Provides a way to send signals via file descriptors. The important part
    is that grid_efd_getfd() returns an actual OS-level file descriptor that
    you can poll on to wait for the event. */

#include "fd.h"

#if defined GRID_HAVE_EVENTFD
#include "efd_eventfd.h"
#elif defined GRID_HAVE_PIPE
#include "efd_pipe.h"
#elif defined GRID_HAVE_SOCKETPAIR
#include "efd_socketpair.h"
#else
#error
#endif

/*  Initialise the efd object. */
int grid_efd_init (struct grid_efd *self);

/*  Uninitialise the efd object. */
void grid_efd_term (struct grid_efd *self);

/*  Get the OS file descriptor that is readable when the efd object
    is signaled. */
grid_fd grid_efd_getfd (struct grid_efd *self);

/*  Stop the efd object. */
void grid_efd_stop (struct grid_efd *self);

/*  Switch the object into signaled state. */
void grid_efd_signal (struct grid_efd *self);

/*  Switch the object into unsignaled state. */
void grid_efd_unsignal (struct grid_efd *self);

/*  Wait till efd object becomes signaled or when timeout (in milliseconds,
    nagative value meaning 'infinite') expires. In the former case 0 is
    returened. In the latter, -ETIMEDOUT. */
int grid_efd_wait (struct grid_efd *self, int timeout);

#endif


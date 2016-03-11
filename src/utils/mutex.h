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

#ifndef GRID_MUTEX_INCLUDED
#define GRID_MUTEX_INCLUDED

#include <pthread.h>

struct grid_mutex {
    pthread_mutex_t mutex;
};

/*  Initialise the mutex. */
void grid_mutex_init (struct grid_mutex *self);

/*  Terminate the mutex. */
void grid_mutex_term (struct grid_mutex *self);

/*  Lock the mutex. Behaviour of multiple locks from the same thread is
    undefined. */
void grid_mutex_lock (struct grid_mutex *self);

/*  Unlock the mutex. Behaviour of unlocking an unlocked mutex is undefined */
void grid_mutex_unlock (struct grid_mutex *self);

#endif

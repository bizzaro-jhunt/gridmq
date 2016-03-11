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

#ifndef GRID_SEM_INCLUDED
#define GRID_SEM_INCLUDED

/*  Simple semaphore. It can have only two values (0/1 i.e. locked/unlocked). */

struct grid_sem;

/*  Initialise the sem object. It is created in locked state. */
void grid_sem_init (struct grid_sem *self);

/*  Uninitialise the sem object. */
void grid_sem_term (struct grid_sem *self);

/*  Unlock the semaphore. */
void grid_sem_post (struct grid_sem *self);

/*  Waits till sem object becomes unlocked and locks it. */
int grid_sem_wait (struct grid_sem *self);

#if defined GRID_HAVE_OSX

#include <pthread.h>

struct grid_sem {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int signaled;
};

#elif defined GRID_HAVE_WINDOWS

#include "win.h"

struct grid_sem {
    HANDLE h;
};

#elif defined GRID_HAVE_SEMAPHORE

#include <semaphore.h>

struct grid_sem {
    sem_t sem;
};

#endif

#endif


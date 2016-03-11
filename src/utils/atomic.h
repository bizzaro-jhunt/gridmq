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

#ifndef GRID_ATOMIC_INCLUDED
#define GRID_ATOMIC_INCLUDED

#if GRID_HAVE_ATOMIC_SOLARIS
#include <atomic.h>
#define GRID_ATOMIC_SOLARIS
#elif defined GRID_HAVE_GCC_ATOMIC_BUILTINS
#define GRID_ATOMIC_GCC_BUILTINS
#else
#include "mutex.h"
#define GRID_ATOMIC_MUTEX
#endif

#include "int.h"

struct grid_atomic {
#if defined GRID_ATOMIC_MUTEX
    struct grid_mutex sync;
#endif
    volatile uint32_t n;
};

/*  Initialise the object. Set it to value 'n'. */
void grid_atomic_init (struct grid_atomic *self, uint32_t n);

/*  Destroy the object. */
void grid_atomic_term (struct grid_atomic *self);

/*  Atomically add n to the object, return old value of the object. */
uint32_t grid_atomic_inc (struct grid_atomic *self, uint32_t n);

/*  Atomically subtract n from the object, return old value of the object. */
uint32_t grid_atomic_dec (struct grid_atomic *self, uint32_t n);

#endif


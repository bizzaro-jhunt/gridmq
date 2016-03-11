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

#include "backoff.h"

void grid_backoff_init (struct grid_backoff *self, int src, int minivl, int maxivl,
    struct grid_fsm *owner)
{
    grid_timer_init (&self->timer, src, owner);
    self->minivl = minivl;
    self->maxivl = maxivl;
    self->n = 1;
}

void grid_backoff_term (struct grid_backoff *self)
{
    grid_timer_term (&self->timer);
}

int grid_backoff_isidle (struct grid_backoff *self)
{
    return grid_timer_isidle (&self->timer);
}

void grid_backoff_start (struct grid_backoff *self)
{
     int timeout;

     /*  Start the timer for the actual n value. If the interval haven't yet
         exceeded the maximum, double the next timeout value. */
     timeout = (self->n - 1) * self->minivl;
     if (timeout > self->maxivl)
         timeout = self->maxivl;
     else
         self->n *= 2;
     grid_timer_start (&self->timer, timeout);
}

void grid_backoff_stop (struct grid_backoff *self)
{
    grid_timer_stop (&self->timer);
}

void grid_backoff_reset (struct grid_backoff *self)
{
    self->n = 1;
}


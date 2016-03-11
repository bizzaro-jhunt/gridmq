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

#ifndef GRID_TIMERSET_INCLUDED
#define GRID_TIMERSET_INCLUDED

#include "../utils/clock.h"
#include "../utils/list.h"

/*  This class stores a list of timeouts and reports the next one to expire
    along with the time till it happens. */

struct grid_timerset_hndl {
    struct grid_list_item list;
    uint64_t timeout;
};

struct grid_timerset {
    struct grid_clock clock;
    struct grid_list timeouts;
};

void grid_timerset_init (struct grid_timerset *self);
void grid_timerset_term (struct grid_timerset *self);
int grid_timerset_add (struct grid_timerset *self, int timeout,
    struct grid_timerset_hndl *hndl);
int grid_timerset_rm (struct grid_timerset *self, struct grid_timerset_hndl *hndl);
int grid_timerset_timeout (struct grid_timerset *self);
int grid_timerset_event (struct grid_timerset *self, struct grid_timerset_hndl **hndl);

void grid_timerset_hndl_init (struct grid_timerset_hndl *self);
void grid_timerset_hndl_term (struct grid_timerset_hndl *self);
int grid_timerset_hndl_isactive (struct grid_timerset_hndl *self);

#endif


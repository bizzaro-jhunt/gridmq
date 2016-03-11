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

#ifndef GRID_ERR_INCLUDED
#define GRID_ERR_INCLUDED

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*  Include grid.h header to define gridmq-specific error codes. */
#include "../grid.h"

#include "fast.h"

#if defined __GNUC__
#define GRID_NORETURN __attribute__ ((noreturn))
#else
#define GRID_NORETURN
#endif

/*  Same as system assert(). However, under Win32 assert has some deficiencies.
    Thus this macro. */
#define grid_assert(x) \
    do {\
        if (grid_slow (!(x))) {\
            fprintf (stderr, "Assertion failed: %s (%s:%d)\n", #x, \
                __FILE__, __LINE__);\
            fflush (stderr);\
            grid_err_abort ();\
        }\
    } while (0)

#define grid_assert_state(obj, state_name) \
    do {\
        if (grid_slow ((obj)->state != state_name)) {\
            fprintf (stderr, \
                "Assertion failed: %d == %s (%s:%d)\n", \
                (obj)->state, #state_name, \
                __FILE__, __LINE__);\
            fflush (stderr);\
            grid_err_abort ();\
        }\
    } while (0)

/*  Checks whether memory allocation was successful. */
#define alloc_assert(x) \
    do {\
        if (grid_slow (!x)) {\
            fprintf (stderr, "Out of memory (%s:%d)\n",\
                __FILE__, __LINE__);\
            fflush (stderr);\
            grid_err_abort ();\
        }\
    } while (0)

/*  Check the condition. If false prints out the errno. */
#define errno_assert(x) \
    do {\
        if (grid_slow (!(x))) {\
            fprintf (stderr, "%s [%d] (%s:%d)\n", grid_err_strerror (errno),\
                (int) errno, __FILE__, __LINE__);\
            fflush (stderr);\
            grid_err_abort ();\
        }\
    } while (0)

/*  Checks whether supplied errno number is an error. */
#define errnum_assert(cond, err) \
    do {\
        if (grid_slow (!(cond))) {\
            fprintf (stderr, "%s [%d] (%s:%d)\n", grid_err_strerror (err),\
                (int) (err), __FILE__, __LINE__);\
            fflush (stderr);\
            grid_err_abort ();\
        }\
    } while (0)

/*  Assertion-like macros for easier fsm debugging. */
#define grid_fsm_error(message, state, src, type) \
    do {\
        fprintf (stderr, "%s: state=%d source=%d action=%d (%s:%d)\n", \
            message, state, src, type, __FILE__, __LINE__);\
        fflush (stderr);\
        grid_err_abort ();\
    } while (0)

#define grid_fsm_bad_action(state, src, type) grid_fsm_error(\
    "Unexpected action", state, src, type)
#define grid_fsm_bad_state(state, src, type) grid_fsm_error(\
    "Unexpected state", state, src, type)
#define grid_fsm_bad_source(state, src, type) grid_fsm_error(\
    "Unexpected source", state, src, type)

/*  Compile-time assert. */
#define CT_ASSERT_HELPER2(prefix, line) prefix##line
#define CT_ASSERT_HELPER1(prefix, line) CT_ASSERT_HELPER2(prefix, line)
#if defined __COUNTER__
#define CT_ASSERT(x) \
    typedef int CT_ASSERT_HELPER1(ct_assert_,__COUNTER__) [(x) ? 1 : -1]
#else
#define CT_ASSERT(x) \
    typedef int CT_ASSERT_HELPER1(ct_assert_,__LINE__) [(x) ? 1 : -1]
#endif

GRID_NORETURN void grid_err_abort (void);
int grid_err_errno (void);
const char *grid_err_strerror (int errnum);


#endif

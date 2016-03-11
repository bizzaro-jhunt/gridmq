/*
    Copyright (c) 2012-2014 Martin Sustrik  All rights reserved.

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

#include "atcpmux.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/attr.h"

#define GRID_ATCPMUX_STATE_IDLE 1
#define GRID_ATCPMUX_STATE_ACTIVE 2
#define GRID_ATCPMUX_STATE_STOPPING_STCPMUX 3
#define GRID_ATCPMUX_STATE_STOPPING_USOCK 4
#define GRID_ATCPMUX_STATE_DONE 5
#define GRID_ATCPMUX_STATE_STOPPING_STCPMUX_FINAL 6
#define GRID_ATCPMUX_STATE_STOPPING 7

#define GRID_ATCPMUX_SRC_USOCK 1
#define GRID_ATCPMUX_SRC_STCPMUX 2

/*  Private functions. */
static void grid_atcpmux_handler (struct grid_fsm *self, int src, int type,
    void *srcptr);
static void grid_atcpmux_shutdown (struct grid_fsm *self, int src, int type,
    void *srcptr);

void grid_atcpmux_init (struct grid_atcpmux *self, int src,
    struct grid_epbase *epbase, struct grid_fsm *owner)
{
    grid_fsm_init (&self->fsm, grid_atcpmux_handler, grid_atcpmux_shutdown,
        src, self, owner);
    self->state = GRID_ATCPMUX_STATE_IDLE;
    self->epbase = epbase;
    grid_usock_init (&self->usock, GRID_ATCPMUX_SRC_USOCK, &self->fsm);
    grid_stcpmux_init (&self->stcpmux, GRID_ATCPMUX_SRC_STCPMUX,
        epbase, &self->fsm);
    grid_fsm_event_init (&self->accepted);
    grid_fsm_event_init (&self->done);
    grid_list_item_init (&self->item);
}

void grid_atcpmux_term (struct grid_atcpmux *self)
{
    grid_assert_state (self, GRID_ATCPMUX_STATE_IDLE);

    grid_list_item_term (&self->item);
    grid_fsm_event_term (&self->done);
    grid_fsm_event_term (&self->accepted);
    grid_stcpmux_term (&self->stcpmux);
    grid_usock_term (&self->usock);
    grid_fsm_term (&self->fsm);
}

int grid_atcpmux_isidle (struct grid_atcpmux *self)
{
    return grid_fsm_isidle (&self->fsm);
}

void grid_atcpmux_start (struct grid_atcpmux *self, int fd)
{
    grid_assert_state (self, GRID_ATCPMUX_STATE_IDLE);

    /*  Start the state machine. */
    grid_fsm_start (&self->fsm);

    /*  Start the stcp state machine. */
    grid_usock_start_fd (&self->usock, fd);
    grid_stcpmux_start (&self->stcpmux, &self->usock);
    self->state = GRID_ATCPMUX_STATE_ACTIVE;
}

void grid_atcpmux_stop (struct grid_atcpmux *self)
{
    grid_fsm_stop (&self->fsm);
}

static void grid_atcpmux_shutdown (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_atcpmux *atcpmux;

    atcpmux = grid_cont (self, struct grid_atcpmux, fsm);

    if (grid_slow (src == GRID_FSM_ACTION && type == GRID_FSM_STOP)) {
        if (!grid_stcpmux_isidle (&atcpmux->stcpmux)) {
            grid_epbase_stat_increment (atcpmux->epbase,
                GRID_STAT_DROPPED_CONNECTIONS, 1);
            grid_stcpmux_stop (&atcpmux->stcpmux);
        }
        atcpmux->state = GRID_ATCPMUX_STATE_STOPPING_STCPMUX_FINAL;
    }
    if (grid_slow (atcpmux->state == GRID_ATCPMUX_STATE_STOPPING_STCPMUX_FINAL)) {
        if (!grid_stcpmux_isidle (&atcpmux->stcpmux))
            return;
        grid_usock_stop (&atcpmux->usock);
        atcpmux->state = GRID_ATCPMUX_STATE_STOPPING;
    }
    if (grid_slow (atcpmux->state == GRID_ATCPMUX_STATE_STOPPING)) {
        if (!grid_usock_isidle (&atcpmux->usock))
            return;
        atcpmux->state = GRID_ATCPMUX_STATE_IDLE;
        grid_fsm_stopped (&atcpmux->fsm, GRID_ATCPMUX_STOPPED);
        return;
    }

    grid_fsm_bad_action(atcpmux->state, src, type);
}

static void grid_atcpmux_handler (struct grid_fsm *self, int src, int type,
    GRID_UNUSED void *srcptr)
{
    struct grid_atcpmux *atcpmux;

    atcpmux = grid_cont (self, struct grid_atcpmux, fsm);

    switch (atcpmux->state) {

/******************************************************************************/
/*  IDLE state.                                                               */
/*  The state machine wasn't yet started.                                     */
/******************************************************************************/
    case GRID_ATCPMUX_STATE_IDLE:
        switch (src) {

        case GRID_FSM_ACTION:
            switch (type) {
            case GRID_FSM_START:
                // TODO
                atcpmux->state = GRID_ATCPMUX_STATE_ACTIVE;
                return;
            default:
                grid_fsm_bad_action (atcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (atcpmux->state, src, type);
        }

/******************************************************************************/
/*  ACTIVE state.                                                             */
/******************************************************************************/
    case GRID_ATCPMUX_STATE_ACTIVE:
        switch (src) {

        case GRID_ATCPMUX_SRC_STCPMUX:
            switch (type) {
            case GRID_STCPMUX_ERROR:
                grid_stcpmux_stop (&atcpmux->stcpmux);
                atcpmux->state = GRID_ATCPMUX_STATE_STOPPING_STCPMUX;
                grid_epbase_stat_increment (atcpmux->epbase,
                    GRID_STAT_BROKEN_CONNECTIONS, 1);
                return;
            default:
                grid_fsm_bad_action (atcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (atcpmux->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_STCPMUX state.                                                   */
/******************************************************************************/
    case GRID_ATCPMUX_STATE_STOPPING_STCPMUX:
        switch (src) {

        case GRID_ATCPMUX_SRC_STCPMUX:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_STCPMUX_STOPPED:
                grid_usock_stop (&atcpmux->usock);
                atcpmux->state = GRID_ATCPMUX_STATE_STOPPING_USOCK;
                return;
            default:
                grid_fsm_bad_action (atcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (atcpmux->state, src, type);
        }

/******************************************************************************/
/*  STOPPING_USOCK state.                                                      */
/******************************************************************************/
    case GRID_ATCPMUX_STATE_STOPPING_USOCK:
        switch (src) {

        case GRID_ATCPMUX_SRC_USOCK:
            switch (type) {
            case GRID_USOCK_SHUTDOWN:
                return;
            case GRID_USOCK_STOPPED:
                grid_fsm_raise (&atcpmux->fsm, &atcpmux->done, GRID_ATCPMUX_ERROR);
                atcpmux->state = GRID_ATCPMUX_STATE_DONE;
                return;
            default:
                grid_fsm_bad_action (atcpmux->state, src, type);
            }

        default:
            grid_fsm_bad_source (atcpmux->state, src, type);
        }

/******************************************************************************/
/*  Invalid state.                                                            */
/******************************************************************************/
    default:
        grid_fsm_bad_state (atcpmux->state, src, type);
    }
}


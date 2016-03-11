/*
    Copyright (c) 2013 Insollo Entertainment, LLC.  All rights reserved.

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
#include "../src/pubsub.h"
#include "../src/pipeline.h"
#include "../src/bus.h"
#include "../src/pair.h"
#include "../src/survey.h"
#include "../src/reqrep.h"

#include "options.h"
#include "../src/utils/sleep.c"
#include "../src/utils/clock.c"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>

enum echo_format {
    GRID_NO_ECHO,
    GRID_ECHO_RAW,
    GRID_ECHO_ASCII,
    GRID_ECHO_QUOTED,
    GRID_ECHO_MSGPACK,
    GRID_ECHO_HEX
};

typedef struct grid_options {
    /* Global options */
    int verbose;

    /* Socket options */
    int socket_type;
    struct grid_string_list bind_addresses;
    struct grid_string_list connect_addresses;
    float send_timeout;
    float recv_timeout;
    struct grid_string_list subscriptions;
    char *socket_name;

    /* Output options */
    float send_delay;
    float send_interval;
    struct grid_blob data_to_send;

    /* Input options */
    enum echo_format echo_format;
} grid_options_t;

/*  Constants to get address of in option declaration  */
static const int grid_push = GRID_PUSH;
static const int grid_pull = GRID_PULL;
static const int grid_pub = GRID_PUB;
static const int grid_sub = GRID_SUB;
static const int grid_req = GRID_REQ;
static const int grid_rep = GRID_REP;
static const int grid_bus = GRID_BUS;
static const int grid_pair = GRID_PAIR;
static const int grid_surveyor = GRID_SURVEYOR;
static const int grid_respondent = GRID_RESPONDENT;


struct grid_enum_item socket_types[] = {
    {"PUSH", GRID_PUSH},
    {"PULL", GRID_PULL},
    {"PUB", GRID_PUB},
    {"SUB", GRID_SUB},
    {"REQ", GRID_REQ},
    {"REP", GRID_REP},
    {"BUS", GRID_BUS},
    {"PAIR", GRID_PAIR},
    {"SURVEYOR", GRID_SURVEYOR},
    {"RESPONDENT", GRID_RESPONDENT},
    {NULL, 0},
};


/*  Constants to get address of in option declaration  */
static const int grid_echo_raw = GRID_ECHO_RAW;
static const int grid_echo_ascii = GRID_ECHO_ASCII;
static const int grid_echo_quoted = GRID_ECHO_QUOTED;
static const int grid_echo_msgpack = GRID_ECHO_MSGPACK;
static const int grid_echo_hex = GRID_ECHO_HEX;

struct grid_enum_item echo_formats[] = {
    {"no", GRID_NO_ECHO},
    {"raw", GRID_ECHO_RAW},
    {"ascii", GRID_ECHO_ASCII},
    {"quoted", GRID_ECHO_QUOTED},
    {"msgpack", GRID_ECHO_MSGPACK},
    {"hex", GRID_ECHO_HEX},
    {NULL, 0},
};

/*  Constants for conflict masks  */
#define GRID_MASK_SOCK 1
#define GRID_MASK_WRITEABLE 2
#define GRID_MASK_READABLE 4
#define GRID_MASK_SOCK_SUB 8
#define GRID_MASK_DATA 16
#define GRID_MASK_ENDPOINT 32
#define GRID_NO_PROVIDES 0
#define GRID_NO_CONFLICTS 0
#define GRID_NO_REQUIRES 0
#define GRID_MASK_SOCK_WRITEABLE (GRID_MASK_SOCK | GRID_MASK_WRITEABLE)
#define GRID_MASK_SOCK_READABLE (GRID_MASK_SOCK | GRID_MASK_READABLE)
#define GRID_MASK_SOCK_READWRITE  (GRID_MASK_SOCK_WRITEABLE|GRID_MASK_SOCK_READABLE)

struct grid_option grid_options[] = {
    /* Generic options */
    {"verbose", 'v', NULL,
     GRID_OPT_INCREMENT, offsetof (grid_options_t, verbose), NULL,
     GRID_NO_PROVIDES, GRID_NO_CONFLICTS, GRID_NO_REQUIRES,
     "Generic", NULL, "Increase verbosity of the gridcat"},
    {"silent", 'q', NULL,
     GRID_OPT_DECREMENT, offsetof (grid_options_t, verbose), NULL,
     GRID_NO_PROVIDES, GRID_NO_CONFLICTS, GRID_NO_REQUIRES,
     "Generic", NULL, "Decrease verbosity of the gridcat"},
    {"help", 'h', NULL,
     GRID_OPT_HELP, 0, NULL,
     GRID_NO_PROVIDES, GRID_NO_CONFLICTS, GRID_NO_REQUIRES,
     "Generic", NULL, "This help text"},

    /* Socket types */
    {"push", 0, "grid_push",
     GRID_OPT_SET_ENUM, offsetof (grid_options_t, socket_type), &grid_push,
     GRID_MASK_SOCK_WRITEABLE, GRID_MASK_SOCK, GRID_MASK_DATA,
     "Socket Types", NULL, "Use GRID_PUSH socket type"},
    {"pull", 0, "grid_pull",
     GRID_OPT_SET_ENUM, offsetof (grid_options_t, socket_type), &grid_pull,
     GRID_MASK_SOCK_READABLE, GRID_MASK_SOCK, GRID_NO_REQUIRES,
     "Socket Types", NULL, "Use GRID_PULL socket type"},
    {"pub", 0, "grid_pub",
     GRID_OPT_SET_ENUM, offsetof (grid_options_t, socket_type), &grid_pub,
     GRID_MASK_SOCK_WRITEABLE, GRID_MASK_SOCK, GRID_MASK_DATA,
     "Socket Types", NULL, "Use GRID_PUB socket type"},
    {"sub", 0, "grid_sub",
     GRID_OPT_SET_ENUM, offsetof (grid_options_t, socket_type), &grid_sub,
     GRID_MASK_SOCK_READABLE|GRID_MASK_SOCK_SUB, GRID_MASK_SOCK, GRID_NO_REQUIRES,
     "Socket Types", NULL, "Use GRID_SUB socket type"},
    {"req", 0, "grid_req",
     GRID_OPT_SET_ENUM, offsetof (grid_options_t, socket_type), &grid_req,
     GRID_MASK_SOCK_READWRITE, GRID_MASK_SOCK, GRID_MASK_DATA,
     "Socket Types", NULL, "Use GRID_REQ socket type"},
    {"rep", 0, "grid_rep",
     GRID_OPT_SET_ENUM, offsetof (grid_options_t, socket_type), &grid_rep,
     GRID_MASK_SOCK_READWRITE, GRID_MASK_SOCK, GRID_NO_REQUIRES,
     "Socket Types", NULL, "Use GRID_REP socket type"},
    {"surveyor", 0, "grid_surveyor",
     GRID_OPT_SET_ENUM, offsetof (grid_options_t, socket_type), &grid_surveyor,
     GRID_MASK_SOCK_READWRITE, GRID_MASK_SOCK, GRID_MASK_DATA,
     "Socket Types", NULL, "Use GRID_SURVEYOR socket type"},
    {"respondent", 0, "grid_respondent",
     GRID_OPT_SET_ENUM, offsetof (grid_options_t, socket_type), &grid_respondent,
     GRID_MASK_SOCK_READWRITE, GRID_MASK_SOCK, GRID_NO_REQUIRES,
     "Socket Types", NULL, "Use GRID_RESPONDENT socket type"},
    {"bus", 0, "grid_bus",
     GRID_OPT_SET_ENUM, offsetof (grid_options_t, socket_type), &grid_bus,
     GRID_MASK_SOCK_READWRITE, GRID_MASK_SOCK, GRID_NO_REQUIRES,
     "Socket Types", NULL, "Use GRID_BUS socket type"},
    {"pair", 0, "grid_pair",
     GRID_OPT_SET_ENUM, offsetof (grid_options_t, socket_type), &grid_pair,
     GRID_MASK_SOCK_READWRITE, GRID_MASK_SOCK, GRID_NO_REQUIRES,
     "Socket Types", NULL, "Use GRID_PAIR socket type"},

    /* Socket Options */
    {"bind", 0, NULL,
     GRID_OPT_LIST_APPEND, offsetof (grid_options_t, bind_addresses), NULL,
     GRID_MASK_ENDPOINT, GRID_NO_CONFLICTS, GRID_NO_REQUIRES,
     "Socket Options", "ADDR", "Bind socket to the address ADDR"},
    {"connect", 0, NULL,
     GRID_OPT_LIST_APPEND, offsetof (grid_options_t, connect_addresses), NULL,
     GRID_MASK_ENDPOINT, GRID_NO_CONFLICTS, GRID_NO_REQUIRES,
     "Socket Options", "ADDR", "Connect socket to the address ADDR"},
    {"bind-ipc", 'X' , NULL, GRID_OPT_LIST_APPEND_FMT,
     offsetof (grid_options_t, bind_addresses), "ipc://%s",
     GRID_MASK_ENDPOINT, GRID_NO_CONFLICTS, GRID_NO_REQUIRES,
     "Socket Options", "PATH", "Bind socket to the ipc address "
                               "\"ipc://PATH\"."},
    {"connect-ipc", 'x' , NULL, GRID_OPT_LIST_APPEND_FMT,
     offsetof (grid_options_t, connect_addresses), "ipc://%s",
     GRID_MASK_ENDPOINT, GRID_NO_CONFLICTS, GRID_NO_REQUIRES,
     "Socket Options", "PATH", "Connect socket to the ipc address "
                               "\"ipc://PATH\"."},
    {"bind-local", 'L' , NULL, GRID_OPT_LIST_APPEND_FMT,
     offsetof (grid_options_t, bind_addresses), "tcp://127.0.0.1:%s",
     GRID_MASK_ENDPOINT, GRID_NO_CONFLICTS, GRID_NO_REQUIRES,
     "Socket Options", "PORT", "Bind socket to the tcp address "
                               "\"tcp://127.0.0.1:PORT\"."},
    {"connect-local", 'l' , NULL, GRID_OPT_LIST_APPEND_FMT,
     offsetof (grid_options_t, connect_addresses), "tcp://127.0.0.1:%s",
     GRID_MASK_ENDPOINT, GRID_NO_CONFLICTS, GRID_NO_REQUIRES,
     "Socket Options", "PORT", "Connect socket to the tcp address "
                               "\"tcp://127.0.0.1:PORT\"."},
    {"recv-timeout", 0, NULL,
     GRID_OPT_FLOAT, offsetof (grid_options_t, recv_timeout), NULL,
     GRID_NO_PROVIDES, GRID_NO_CONFLICTS, GRID_MASK_READABLE,
     "Socket Options", "SEC", "Set timeout for receiving a message"},
    {"send-timeout", 0, NULL,
     GRID_OPT_FLOAT, offsetof (grid_options_t, send_timeout), NULL,
     GRID_NO_PROVIDES, GRID_NO_CONFLICTS, GRID_MASK_WRITEABLE,
     "Socket Options", "SEC", "Set timeout for sending a message"},
    {"socket-name", 0, NULL,
     GRID_OPT_STRING, offsetof (grid_options_t, socket_name), NULL,
     GRID_NO_PROVIDES, GRID_NO_CONFLICTS, GRID_NO_REQUIRES,
     "Socket Options", "NAME", "Name of the socket for statistics"},

    /* Pattern-specific options */
    {"subscribe", 0, NULL,
     GRID_OPT_LIST_APPEND, offsetof (grid_options_t, subscriptions), NULL,
     GRID_NO_PROVIDES, GRID_NO_CONFLICTS, GRID_MASK_SOCK_SUB,
     "SUB Socket Options", "PREFIX", "Subscribe to the prefix PREFIX. "
        "Note: socket will be subscribed to everything (empty prefix) if "
        "no prefixes are specified on the command-line."},

    /* Input Options */
    {"format", 0, NULL,
     GRID_OPT_ENUM, offsetof (grid_options_t, echo_format), &echo_formats,
     GRID_NO_PROVIDES, GRID_NO_CONFLICTS, GRID_MASK_READABLE,
     "Input Options", "FORMAT", "Use echo format FORMAT "
                               "(same as the options below)"},
    {"raw", 0, NULL,
     GRID_OPT_SET_ENUM, offsetof (grid_options_t, echo_format), &grid_echo_raw,
     GRID_NO_PROVIDES, GRID_NO_CONFLICTS, GRID_MASK_READABLE,
     "Input Options", NULL, "Dump message as is "
                           "(Note: no delimiters are printed)"},
    {"ascii", 'A', NULL,
     GRID_OPT_SET_ENUM, offsetof (grid_options_t, echo_format), &grid_echo_ascii,
     GRID_NO_PROVIDES, GRID_NO_CONFLICTS, GRID_MASK_READABLE,
     "Input Options", NULL, "Print ASCII part of message delimited by newline. "
                           "All non-ascii characters replaced by dot."},
    {"quoted", 'Q', NULL,
     GRID_OPT_SET_ENUM, offsetof (grid_options_t, echo_format), &grid_echo_quoted,
     GRID_NO_PROVIDES, GRID_NO_CONFLICTS, GRID_MASK_READABLE,
     "Input Options", NULL, "Print each message on separate line in double "
                           "quotes with C-like character escaping"},
    {"msgpack", 0, NULL,
     GRID_OPT_SET_ENUM, offsetof (grid_options_t, echo_format), &grid_echo_msgpack,
     GRID_NO_PROVIDES, GRID_NO_CONFLICTS, GRID_MASK_READABLE,
     "Input Options", NULL, "Print each message as msgpacked string (raw type)."
                           " This is useful for programmatic parsing."},

    {"hex", 0, NULL,
     GRID_OPT_SET_ENUM, offsetof (grid_options_t, echo_format), &grid_echo_hex,
     GRID_NO_PROVIDES, GRID_NO_CONFLICTS, GRID_MASK_READABLE,
     "Input Options", NULL, "Print each message on separate line in double "
                           "quotes with hex values"},
    /* Output Options */
    {"interval", 'i', NULL,
     GRID_OPT_FLOAT, offsetof (grid_options_t, send_interval), NULL,
     GRID_NO_PROVIDES, GRID_NO_CONFLICTS, GRID_MASK_WRITEABLE,
     "Output Options", "SEC", "Send message (or request) every SEC seconds"},
    {"delay", 'd', NULL,
     GRID_OPT_FLOAT, offsetof (grid_options_t, send_delay), NULL,
     GRID_NO_PROVIDES, GRID_NO_CONFLICTS, GRID_NO_REQUIRES,
     "Output Options", "SEC", "Wait for SEC seconds before sending message"
                              " (useful for one-shot PUB sockets)"},
    {"data", 'D', NULL,
     GRID_OPT_BLOB, offsetof (grid_options_t, data_to_send), &echo_formats,
     GRID_MASK_DATA, GRID_MASK_DATA, GRID_MASK_WRITEABLE,
     "Output Options", "DATA", "Send DATA to the socket and quit for "
     "PUB, PUSH, PAIR, BUS socket. Use DATA to reply for REP or "
     " RESPONDENT socket. Send DATA as request for REQ or SURVEYOR socket."},
    {"file", 'F', NULL,
     GRID_OPT_READ_FILE, offsetof (grid_options_t, data_to_send), &echo_formats,
     GRID_MASK_DATA, GRID_MASK_DATA, GRID_MASK_WRITEABLE,
     "Output Options", "PATH", "Same as --data but get data from file PATH"},

    /* Sentinel */
    {NULL, 0, NULL,
     0, 0, NULL,
     0, 0, 0,
     NULL, NULL, NULL},
    };


struct grid_commandline grid_cli = {
    "A command-line interface to gridmq",
    "",
    grid_options,
    GRID_MASK_SOCK | GRID_MASK_ENDPOINT,
};


void grid_assert_errno (int flag, char *description)
{
    int err;

    if (!flag) {
        err = errno;
        fprintf (stderr, "%s: %s\n", description, grid_strerror (err));
        exit (3);
    }
}

void grid_sub_init (grid_options_t *options, int sock)
{
    int i;
    int rc;

    if (options->subscriptions.num) {
        for (i = 0; i < options->subscriptions.num; ++i) {
            rc = grid_setsockopt (sock, GRID_SUB, GRID_SUB_SUBSCRIBE,
                options->subscriptions.items[i],
                strlen (options->subscriptions.items[i]));
            grid_assert_errno (rc == 0, "Can't subscribe");
        }
    } else {
        rc = grid_setsockopt (sock, GRID_SUB, GRID_SUB_SUBSCRIBE, "", 0);
        grid_assert_errno (rc == 0, "Can't subscribe");
    }
}

void grid_set_recv_timeout (int sock, int millis)
{
    int rc;
    rc = grid_setsockopt (sock, GRID_SOL_SOCKET, GRID_RCVTIMEO,
                       &millis, sizeof (millis));
    grid_assert_errno (rc == 0, "Can't set recv timeout");
}

int grid_create_socket (grid_options_t *options)
{
    int sock;
    int rc;
    int millis;

    sock = grid_socket (AF_SP, options->socket_type);
    grid_assert_errno (sock >= 0, "Can't create socket");

    /* Generic initialization */
    if (options->send_timeout >= 0) {
        millis = (int)(options->send_timeout * 1000);
        rc = grid_setsockopt (sock, GRID_SOL_SOCKET, GRID_SNDTIMEO,
                           &millis, sizeof (millis));
        grid_assert_errno (rc == 0, "Can't set send timeout");
    }
    if (options->recv_timeout >= 0) {
        grid_set_recv_timeout (sock, (int) options->recv_timeout);
    }
    if (options->socket_name) {
        rc = grid_setsockopt (sock, GRID_SOL_SOCKET, GRID_SOCKET_NAME,
                           options->socket_name, strlen(options->socket_name));
        grid_assert_errno (rc == 0, "Can't set socket name");
    }

    /* Specific initialization */
    switch (options->socket_type) {
    case GRID_SUB:
        grid_sub_init (options, sock);
        break;
    }

    return sock;
}

void grid_print_message (grid_options_t *options, char *buf, int buflen)
{
    switch (options->echo_format) {
    case GRID_NO_ECHO:
        return;
    case GRID_ECHO_RAW:
        fwrite (buf, 1, buflen, stdout);
        break;
    case GRID_ECHO_ASCII:
        for (; buflen > 0; --buflen, ++buf) {
            if (isprint (*buf)) {
                fputc (*buf, stdout);
            } else {
                fputc ('.', stdout);
            }
        }
        fputc ('\n', stdout);
        break;
    case GRID_ECHO_QUOTED:
        fputc ('"', stdout);
        for (; buflen > 0; --buflen, ++buf) {
            switch (*buf) {
            case '\n':
                fprintf (stdout, "\\n");
                break;
            case '\r':
                fprintf (stdout, "\\r");
                break;
            case '\\':
            case '\"':
                fprintf (stdout, "\\%c", *buf);
                break;
            default:
                if (isprint (*buf)) {
                    fputc (*buf, stdout);
                } else {
                    fprintf (stdout, "\\x%02x", (unsigned char)*buf);
                }
            }
        }
        fprintf (stdout, "\"\n");
        break;
    case GRID_ECHO_MSGPACK:
        if (buflen < 256) {
            fputc ('\xc4', stdout);
            fputc (buflen, stdout);
            fwrite (buf, 1, buflen, stdout);
        } else if (buflen < 65536) {
            fputc ('\xc5', stdout);
            fputc (buflen >> 8, stdout);
            fputc (buflen & 0xff, stdout);
            fwrite (buf, 1, buflen, stdout);
        } else {
            fputc ('\xc6', stdout);
            fputc (buflen >> 24, stdout);
            fputc ((buflen >> 16) & 0xff, stdout);
            fputc ((buflen >> 8) & 0xff, stdout);
            fputc (buflen & 0xff, stdout);
            fwrite (buf, 1, buflen, stdout);
        }
        break;
    case GRID_ECHO_HEX:
        fputc ('"', stdout);
        for (; buflen > 0; --buflen, ++buf) {
             fprintf (stdout, "\\x%02x", (unsigned char)*buf);
        }
        fprintf (stdout, "\"\n");
        break;
    
    }
    fflush (stdout);
}

void grid_connect_socket (grid_options_t *options, int sock)
{
    int i;
    int rc;

    for (i = 0; i < options->bind_addresses.num; ++i) {
        rc = grid_bind (sock, options->bind_addresses.items[i]);
        grid_assert_errno (rc >= 0, "Can't bind");
    }
    for (i = 0; i < options->connect_addresses.num; ++i) {
        rc = grid_connect (sock, options->connect_addresses.items[i]);
        grid_assert_errno (rc >= 0, "Can't connect");
    }
}

void grid_send_loop (grid_options_t *options, int sock)
{
    int rc;
    uint64_t start_time;
    int64_t time_to_sleep, interval;
    struct grid_clock clock;

    interval = (int)(options->send_interval*1000);
    grid_clock_init (&clock);

    for (;;) {
        start_time = grid_clock_now (&clock);
        rc = grid_send (sock,
            options->data_to_send.data, options->data_to_send.length,
            0);
        if (rc < 0 && errno == EAGAIN) {
            fprintf (stderr, "Message not sent (EAGAIN)\n");
        } else {
            grid_assert_errno (rc >= 0, "Can't send");
        }
        if (interval >= 0) {
            time_to_sleep = (start_time + interval) - grid_clock_now (&clock);
            if (time_to_sleep > 0) {
                grid_sleep ((int) time_to_sleep);
            }
        } else {
            break;
        }
    }

    grid_clock_term(&clock);
}

void grid_recv_loop (grid_options_t *options, int sock)
{
    int rc;
    void *buf;

    for (;;) {
        rc = grid_recv (sock, &buf, GRID_MSG, 0);
        if (rc < 0 && errno == EAGAIN) {
            continue;
        } else if (rc < 0 && (errno == ETIMEDOUT || errno == EFSM)) {
            return;  /*  No more messages possible  */
        } else {
            grid_assert_errno (rc >= 0, "Can't recv");
        }
        grid_print_message (options, buf, rc);
        grid_freemsg (buf);
    }
}

void grid_rw_loop (grid_options_t *options, int sock)
{
    int rc;
    void *buf;
    uint64_t start_time;
    int64_t time_to_sleep, interval, recv_timeout;
    struct grid_clock clock;

    interval = (int)(options->send_interval*1000);
    recv_timeout = (int)(options->recv_timeout*1000);
    grid_clock_init (&clock);

    for (;;) {
        start_time = grid_clock_now (&clock);
        rc = grid_send (sock,
            options->data_to_send.data, options->data_to_send.length,
            0);
        if (rc < 0 && errno == EAGAIN) {
            fprintf (stderr, "Message not sent (EAGAIN)\n");
        } else {
            grid_assert_errno (rc >= 0, "Can't send");
        }
        if (options->send_interval < 0) {  /*  Never send any more  */
            grid_recv_loop (options, sock);
            return;
        }

        for (;;) {
            time_to_sleep = (start_time + interval) - grid_clock_now (&clock);
            if (time_to_sleep <= 0) {
                break;
            }
            if (recv_timeout >= 0 && time_to_sleep > recv_timeout)
            {
                time_to_sleep = recv_timeout;
            }
            grid_set_recv_timeout (sock, (int) time_to_sleep);
            rc = grid_recv (sock, &buf, GRID_MSG, 0);
            if (rc < 0) {
                if (errno == EAGAIN) {
                    continue;
                } else if (errno == ETIMEDOUT || errno == EFSM) {
                    time_to_sleep = (start_time + interval)
                        - grid_clock_now (&clock);
                    if (time_to_sleep > 0)
                        grid_sleep ((int) time_to_sleep);
                    continue;
                }
            }
            grid_assert_errno (rc >= 0, "Can't recv");
            grid_print_message (options, buf, rc);
            grid_freemsg (buf);
        }
    }

    grid_clock_term(&clock);
}

void grid_resp_loop (grid_options_t *options, int sock)
{
    int rc;
    void *buf;

    for (;;) {
        rc = grid_recv (sock, &buf, GRID_MSG, 0);
        if (rc < 0 && errno == EAGAIN) {
                continue;
        } else {
            grid_assert_errno (rc >= 0, "Can't recv");
        }
        grid_print_message (options, buf, rc);
        grid_freemsg (buf);
        rc = grid_send (sock,
            options->data_to_send.data, options->data_to_send.length,
            0);
        if (rc < 0 && errno == EAGAIN) {
            fprintf (stderr, "Message not sent (EAGAIN)\n");
        } else {
            grid_assert_errno (rc >= 0, "Can't send");
        }
    }
}

int main (int argc, char **argv)
{
    int sock;
    grid_options_t options = {
        /* verbose           */ 0,
        /* socket_type       */ 0,
        /* bind_addresses    */ {NULL, NULL, 0, 0},
        /* connect_addresses */ {NULL, NULL, 0, 0},
        /* send_timeout      */ -1.f,
        /* recv_timeout      */ -1.f,
        /* subscriptions     */ {NULL, NULL, 0, 0},
        /* socket_name       */ NULL,
        /* send_delay        */ 0.f,
        /* send_interval     */ -1.f,
        /* data_to_send      */ {NULL, 0, 0},
        /* echo_format       */ GRID_NO_ECHO
    };

    grid_parse_options (&grid_cli, &options, argc, argv);
    sock = grid_create_socket (&options);
    grid_connect_socket (&options, sock);
    grid_sleep((int)(options.send_delay*1000));
    switch (options.socket_type) {
    case GRID_PUB:
    case GRID_PUSH:
        grid_send_loop (&options, sock);
        break;
    case GRID_SUB:
    case GRID_PULL:
        grid_recv_loop (&options, sock);
        break;
    case GRID_BUS:
    case GRID_PAIR:
        if (options.data_to_send.data) {
            grid_rw_loop (&options, sock);
        } else {
            grid_recv_loop (&options, sock);
        }
        break;
    case GRID_SURVEYOR:
    case GRID_REQ:
        grid_rw_loop (&options, sock);
        break;
    case GRID_REP:
    case GRID_RESPONDENT:
        if (options.data_to_send.data) {
            grid_resp_loop (&options, sock);
        } else {
            grid_recv_loop (&options, sock);
        }
        break;
    }

    grid_close (sock);
    grid_free_options(&grid_cli, &options);
    return 0;
}

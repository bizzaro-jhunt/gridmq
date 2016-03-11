/*
    Copyright (c) 2012-2014 Martin Sustrik  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.
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

#ifndef GRID_H_INCLUDED
#define GRID_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <stddef.h>

/*  Handle DSO symbol visibility                                             */
#if defined GRID_NO_EXPORTS
#   define GRID_EXPORT
#else
#    if defined __SUNPRO_C
#        define GRID_EXPORT __global
#    elif (defined __GNUC__ && __GNUC__ >= 4) || \
           defined __INTEL_COMPILER || defined __clang__
#        define GRID_EXPORT __attribute__ ((visibility("default")))
#    else
#        define GRID_EXPORT
#    endif
#endif

/******************************************************************************/
/*  ABI versioning support.                                                   */
/******************************************************************************/

/*  Don't change this unless you know exactly what you're doing and have      */
/*  read and understand the following documents:                              */
/*  www.gnu.org/software/libtool/manual/html_node/Libtool-versioning.html     */
/*  www.gnu.org/software/libtool/manual/html_node/Updating-version-info.html  */

/*  The current interface version. */
#define GRID_VERSION_CURRENT 4

/*  The latest revision of the current interface. */
#define GRID_VERSION_REVISION 0

/*  How many past interface versions are still supported. */
#define GRID_VERSION_AGE 0

/******************************************************************************/
/*  Errors.                                                                   */
/******************************************************************************/

/*  A number random enough not to collide with different errno ranges on      */
/*  different OSes. The assumption is that error_t is at least 32-bit type.   */
#define GRID_HAUSNUMERO 156384712

/*  On some platforms some standard POSIX errnos are not defined.    */
#ifndef ENOTSUP
#define ENOTSUP (GRID_HAUSNUMERO + 1)
#endif
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT (GRID_HAUSNUMERO + 2)
#endif
#ifndef ENOBUFS
#define ENOBUFS (GRID_HAUSNUMERO + 3)
#endif
#ifndef ENETDOWN
#define ENETDOWN (GRID_HAUSNUMERO + 4)
#endif
#ifndef EADDRINUSE
#define EADDRINUSE (GRID_HAUSNUMERO + 5)
#endif
#ifndef EADDRNOTAVAIL
#define EADDRNOTAVAIL (GRID_HAUSNUMERO + 6)
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED (GRID_HAUSNUMERO + 7)
#endif
#ifndef EINPROGRESS
#define EINPROGRESS (GRID_HAUSNUMERO + 8)
#endif
#ifndef ENOTSOCK
#define ENOTSOCK (GRID_HAUSNUMERO + 9)
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT (GRID_HAUSNUMERO + 10)
#endif
#ifndef EPROTO
#define EPROTO (GRID_HAUSNUMERO + 11)
#endif
#ifndef EAGAIN
#define EAGAIN (GRID_HAUSNUMERO + 12)
#endif
#ifndef EBADF
#define EBADF (GRID_HAUSNUMERO + 13)
#endif
#ifndef EINVAL
#define EINVAL (GRID_HAUSNUMERO + 14)
#endif
#ifndef EMFILE
#define EMFILE (GRID_HAUSNUMERO + 15)
#endif
#ifndef EFAULT
#define EFAULT (GRID_HAUSNUMERO + 16)
#endif
#ifndef EACCES
#define EACCES (GRID_HAUSNUMERO + 17)
#endif
#ifndef EACCESS
#define EACCESS (EACCES)
#endif
#ifndef ENETRESET
#define ENETRESET (GRID_HAUSNUMERO + 18)
#endif
#ifndef ENETUNREACH
#define ENETUNREACH (GRID_HAUSNUMERO + 19)
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH (GRID_HAUSNUMERO + 20)
#endif
#ifndef ENOTCONN
#define ENOTCONN (GRID_HAUSNUMERO + 21)
#endif
#ifndef EMSGSIZE
#define EMSGSIZE (GRID_HAUSNUMERO + 22)
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT (GRID_HAUSNUMERO + 23)
#endif
#ifndef ECONNABORTED
#define ECONNABORTED (GRID_HAUSNUMERO + 24)
#endif
#ifndef ECONNRESET
#define ECONNRESET (GRID_HAUSNUMERO + 25)
#endif
#ifndef ENOPROTOOPT
#define ENOPROTOOPT (GRID_HAUSNUMERO + 26)
#endif
#ifndef EISCONN
#define EISCONN (GRID_HAUSNUMERO + 27)
#define GRID_EISCONN_DEFINED
#endif
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT (GRID_HAUSNUMERO + 28)
#endif

/*  Native gridmq error codes.                                               */
#ifndef ETERM
#define ETERM (GRID_HAUSNUMERO + 53)
#endif
#ifndef EFSM
#define EFSM (GRID_HAUSNUMERO + 54)
#endif

/*  This function retrieves the errno as it is known to the library.          */
/*  The goal of this function is to make the code 100% portable, including    */
/*  where the library is compiled with certain CRT library (on Windows) and   */
/*  linked to an application that uses different CRT library.                 */
GRID_EXPORT int grid_errno (void);

/*  Resolves system errors and native errors to human-readable string.        */
GRID_EXPORT const char *grid_strerror (int errnum);


/*  Returns the symbol name (e.g. "GRID_REQ") and value at a specified index.   */
/*  If the index is out-of-range, returns NULL and sets errno to EINVAL       */
/*  General usage is to start at i=0 and iterate until NULL is returned.      */
GRID_EXPORT const char *grid_symbol (int i, int *value);

/*  Constants that are returned in `ns` member of grid_symbol_properties        */
#define GRID_NS_NAMESPACE 0
#define GRID_NS_VERSION 1
#define GRID_NS_DOMAIN 2
#define GRID_NS_TRANSPORT 3
#define GRID_NS_PROTOCOL 4
#define GRID_NS_OPTION_LEVEL 5
#define GRID_NS_SOCKET_OPTION 6
#define GRID_NS_TRANSPORT_OPTION 7
#define GRID_NS_OPTION_TYPE 8
#define GRID_NS_OPTION_UNIT 9
#define GRID_NS_FLAG 10
#define GRID_NS_ERROR 11
#define GRID_NS_LIMIT 12
#define GRID_NS_EVENT 13

/*  Constants that are returned in `type` member of grid_symbol_properties      */
#define GRID_TYPE_NONE 0
#define GRID_TYPE_INT 1
#define GRID_TYPE_STR 2

/*  Constants that are returned in the `unit` member of grid_symbol_properties  */
#define GRID_UNIT_NONE 0
#define GRID_UNIT_BYTES 1
#define GRID_UNIT_MILLISECONDS 2
#define GRID_UNIT_PRIORITY 3
#define GRID_UNIT_BOOLEAN 4

/*  Structure that is returned from grid_symbol  */
struct grid_symbol_properties {

    /*  The constant value  */
    int value;

    /*  The constant name  */
    const char* name;

    /*  The constant namespace, or zero for namespaces themselves */
    int ns;

    /*  The option type for socket option constants  */
    int type;

    /*  The unit for the option value for socket option constants  */
    int unit;
};

/*  Fills in grid_symbol_properties structure and returns it's length           */
/*  If the index is out-of-range, returns 0                                   */
/*  General usage is to start at i=0 and iterate until zero is returned.      */
GRID_EXPORT int grid_symbol_info (int i,
    struct grid_symbol_properties *buf, int buflen);

/******************************************************************************/
/*  Helper function for shutting down multi-threaded applications.            */
/******************************************************************************/

GRID_EXPORT void grid_term (void);

/******************************************************************************/
/*  Zero-copy support.                                                        */
/******************************************************************************/

#define GRID_MSG ((size_t) -1)

GRID_EXPORT void *grid_allocmsg (size_t size, int type);
GRID_EXPORT void *grid_reallocmsg (void *msg, size_t size);
GRID_EXPORT int grid_freemsg (void *msg);

/******************************************************************************/
/*  Socket definition.                                                        */
/******************************************************************************/

struct grid_iovec {
    void *iov_base;
    size_t iov_len;
};

struct grid_msghdr {
    struct grid_iovec *msg_iov;
    int msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
};

struct grid_cmsghdr {
    size_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
};

/*  Internal stuff. Not to be used directly.                                  */
GRID_EXPORT  struct grid_cmsghdr *grid_cmsg_nxthdr_ (
    const struct grid_msghdr *mhdr,
    const struct grid_cmsghdr *cmsg);
#define GRID_CMSG_ALIGN_(len) \
    (((len) + sizeof (size_t) - 1) & (size_t) ~(sizeof (size_t) - 1))

/* POSIX-defined msghdr manipulation. */

#define GRID_CMSG_FIRSTHDR(mhdr) \
    grid_cmsg_nxthdr_ ((struct grid_msghdr*) (mhdr), NULL)

#define GRID_CMSG_NXTHDR(mhdr, cmsg) \
    grid_cmsg_nxthdr_ ((struct grid_msghdr*) (mhdr), (struct grid_cmsghdr*) (cmsg))

#define GRID_CMSG_DATA(cmsg) \
    ((unsigned char*) (((struct grid_cmsghdr*) (cmsg)) + 1))

/* Extensions to POSIX defined by RFC 3542.                                   */

#define GRID_CMSG_SPACE(len) \
    (GRID_CMSG_ALIGN_ (len) + GRID_CMSG_ALIGN_ (sizeof (struct grid_cmsghdr)))

#define GRID_CMSG_LEN(len) \
    (GRID_CMSG_ALIGN_ (sizeof (struct grid_cmsghdr)) + (len))

/*  SP address families.                                                      */
#define AF_SP 1
#define AF_SP_RAW 2

/*  Max size of an SP address.                                                */
#define GRID_SOCKADDR_MAX 128

/*  Socket option levels: Negative numbers are reserved for transports,
    positive for socket types. */
#define GRID_SOL_SOCKET 0

/*  Generic socket options (GRID_SOL_SOCKET level).                             */
#define GRID_LINGER 1
#define GRID_SNDBUF 2
#define GRID_RCVBUF 3
#define GRID_SNDTIMEO 4
#define GRID_RCVTIMEO 5
#define GRID_RECONNECT_IVL 6
#define GRID_RECONNECT_IVL_MAX 7
#define GRID_SNDPRIO 8
#define GRID_RCVPRIO 9
#define GRID_SNDFD 10
#define GRID_RCVFD 11
#define GRID_DOMAIN 12
#define GRID_PROTOCOL 13
#define GRID_IPV4ONLY 14
#define GRID_SOCKET_NAME 15
#define GRID_RCVMAXSIZE 16

/*  Send/recv options.                                                        */
#define GRID_DONTWAIT 1

/*  Ancillary data.                                                           */
#define PROTO_SP 1
#define SP_HDR 1

GRID_EXPORT int grid_socket (int domain, int protocol);
GRID_EXPORT int grid_close (int s);
GRID_EXPORT int grid_setsockopt (int s, int level, int option, const void *optval,
    size_t optvallen);
GRID_EXPORT int grid_getsockopt (int s, int level, int option, void *optval,
    size_t *optvallen);
GRID_EXPORT int grid_bind (int s, const char *addr);
GRID_EXPORT int grid_connect (int s, const char *addr);
GRID_EXPORT int grid_shutdown (int s, int how);
GRID_EXPORT int grid_send (int s, const void *buf, size_t len, int flags);
GRID_EXPORT int grid_recv (int s, void *buf, size_t len, int flags);
GRID_EXPORT int grid_sendmsg (int s, const struct grid_msghdr *msghdr, int flags);
GRID_EXPORT int grid_recvmsg (int s, struct grid_msghdr *msghdr, int flags);

/******************************************************************************/
/*  Socket mutliplexing support.                                              */
/******************************************************************************/

#define GRID_POLLIN 1
#define GRID_POLLOUT 2

struct grid_pollfd {
    int fd;
    short events;
    short revents;
};

GRID_EXPORT int grid_poll (struct grid_pollfd *fds, int nfds, int timeout);

/******************************************************************************/
/*  Built-in support for devices.                                             */
/******************************************************************************/

GRID_EXPORT int grid_device (int s1, int s2);

/******************************************************************************/
/*  Built-in support for multiplexers.                                        */
/******************************************************************************/

GRID_EXPORT int grid_tcpmuxd (int port);

#ifdef __cplusplus
}
#endif

#endif


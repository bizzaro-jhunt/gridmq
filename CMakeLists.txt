#
#   Copyright (c) 2012 Martin Sustrik  All rights reserved.
#   Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.
#
#   Permission is hereby granted, free of charge, to any person obtaining a copy
#   of this software and associated documentation files (the "Software"),
#   to deal in the Software without restriction, including without limitation
#   the rights to use, copy, modify, merge, publish, distribute, sublicense,
#   and/or sell copies of the Software, and to permit persons to whom
#   the Software is furnished to do so, subject to the following conditions:
#
#   The above copyright notice and this permission notice shall be included
#   in all copies or substantial portions of the Software.
#
#   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
#   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
#   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
#   IN THE SOFTWARE.
#

cmake_minimum_required (VERSION 2.8)
include (CheckIncludeFiles)
include (CheckSymbolExists)
include (CheckCSourceCompiles)

project (gridmq C)
enable_testing ()

#  Platform checks.

if (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
    add_definitions (-DGRID_HAVE_WINDOWS)
    add_definitions (-D_CRT_SECURE_NO_WARNINGS)

    if (MINGW)
        add_definitions (-DGRID_HAVE_MINGW -DGRID_HAVE_STDINT -D_WIN32_WINNT=0x0600)
    endif ()
else ()
    message (FATAL_ERROR "ERROR: CMake build system is intended only to generate MSVC solution files.\nUse autotools build system for any other purpose." )
endif ()

#  Build the library itself.

add_subdirectory (src)

#  Build the tools

add_executable (gridcat tools/gridcat.c tools/options.c)
target_link_libraries (gridcat gridmq)

#  Build unit tests.

set (all_tests "")

macro (add_libgridmq_test NAME TIMEOUT)
    list (APPEND all_tests ${NAME})
    add_executable (${NAME} tests/${NAME}.c)
    target_link_libraries (${NAME} gridmq)
    add_test (NAME ${NAME} COMMAND ${NAME})
    set_tests_properties (${NAME} PROPERTIES TIMEOUT ${TIMEOUT})
endmacro (add_libgridmq_test)

#  Transport tests.
add_libgridmq_test (inproc 5)
add_libgridmq_test (inproc_shutdown 5)
add_libgridmq_test (ipc 5)
add_libgridmq_test (ipc_shutdown 30)
add_libgridmq_test (ipc_stress 5)
add_libgridmq_test (tcp 5)
add_libgridmq_test (tcp_shutdown 120)
add_libgridmq_test (ws 5)
add_libgridmq_test (tcpmux 5)

#  Protocol tests.
add_libgridmq_test (pair 5)
add_libgridmq_test (pubsub 5)
add_libgridmq_test (reqrep 5)
add_libgridmq_test (pipeline 5)
add_libgridmq_test (survey 5)
add_libgridmq_test (bus 5)

#  Feature tests.
add_libgridmq_test (async_shutdown 5)
add_libgridmq_test (block 5)
add_libgridmq_test (term 5)
add_libgridmq_test (timeo 5)
add_libgridmq_test (iovec 5)
add_libgridmq_test (msg 5)
add_libgridmq_test (prio 5)
add_libgridmq_test (poll 5)
add_libgridmq_test (device 5)
add_libgridmq_test (device4 5)
add_libgridmq_test (device5 5)
add_libgridmq_test (device6 5)
add_libgridmq_test (device7 120)
add_libgridmq_test (emfile 5)
add_libgridmq_test (domain 5)
add_libgridmq_test (trie 5)
add_libgridmq_test (list 5)
add_libgridmq_test (hash 5)
add_libgridmq_test (symbol 5)
add_libgridmq_test (separation 5)
add_libgridmq_test (zerocopy 5)
add_libgridmq_test (shutdown 5)
add_libgridmq_test (cmsg 5)
add_libgridmq_test (bug328 5)
add_libgridmq_test (win_sec_attr 5)

#  Build the performance tests.

macro (add_libgridmq_perf NAME)
    add_executable (${NAME} perf/${NAME}.c)
    target_link_libraries (${NAME} gridmq)
endmacro (add_libgridmq_perf)

add_libgridmq_perf (inproc_lat)
add_libgridmq_perf (inproc_thr)
add_libgridmq_perf (local_lat)
add_libgridmq_perf (remote_lat)
add_libgridmq_perf (local_thr)
add_libgridmq_perf (remote_thr)

#  NSIS package

install (FILES src/grid.h DESTINATION include/gridmq)
install (FILES src/inproc.h DESTINATION include/gridmq)
install (FILES src/ipc.h DESTINATION include/gridmq)
install (FILES src/tcp.h DESTINATION include/gridmq)
install (FILES src/ws.h DESTINATION include/gridmq)
install (FILES src/pair.h DESTINATION include/gridmq)
install (FILES src/pubsub.h DESTINATION include/gridmq)
install (FILES src/reqrep.h DESTINATION include/gridmq)
install (FILES src/pipeline.h DESTINATION include/gridmq)
install (FILES src/survey.h DESTINATION include/gridmq)
install (FILES src/bus.h DESTINATION include/gridmq)

install (TARGETS gridcat RUNTIME DESTINATION bin)

set (CPACK_GENERATOR "NSIS")
set (CPACK_PACKAGE_NAME "gridmq")
include (CPack)

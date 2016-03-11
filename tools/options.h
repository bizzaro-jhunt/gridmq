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

#ifndef GRID_OPTIONS_HEADER
#define GRID_OPTIONS_HEADER

enum grid_option_type {
    GRID_OPT_HELP,
    GRID_OPT_INT,
    GRID_OPT_INCREMENT,
    GRID_OPT_DECREMENT,
    GRID_OPT_ENUM,
    GRID_OPT_SET_ENUM,
    GRID_OPT_STRING,
    GRID_OPT_BLOB,
    GRID_OPT_FLOAT,
    GRID_OPT_LIST_APPEND,
    GRID_OPT_LIST_APPEND_FMT,
    GRID_OPT_READ_FILE
};

struct grid_option {
    /*  Option names  */
    char *longname;
    char shortname;
    char *arg0name;

    /*  Parsing specification  */
    enum grid_option_type type;
    int offset;  /*  offsetof() where to store the value  */
    const void *pointer;  /*  type specific pointer  */

    /*  Conflict mask for options  */
    unsigned long mask_set;
    unsigned long conflicts_mask;
    unsigned long requires_mask;

    /*  Group and description for --help  */
    char *group;
    char *metavar;
    char *description;
};

struct grid_commandline {
    char *short_description;
    char *long_description;
    struct grid_option *options;
    int required_options;
};

struct grid_enum_item {
    char *name;
    int value;
};

struct grid_string_list {
    char **items;
    char **to_free;
    int num;
    int to_free_num;
};

struct grid_blob {
    char *data;
    int length;
    int need_free;
};


void grid_parse_options (struct grid_commandline *cline,
                      void *target, int argc, char **argv);
void grid_free_options (struct grid_commandline *cline, void *target);


#endif  /* GRID_OPTIONS_HEADER */

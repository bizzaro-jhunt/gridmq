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

#include "options.h"

#include "../src/utils/err.c"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>

struct grid_parse_context {
    /*  Initial state  */
    struct grid_commandline *def;
    struct grid_option *options;
    void *target;
    int argc;
    char **argv;
    unsigned long requires;

    /*  Current values  */
    unsigned long mask;
    int args_left;
    char **arg;
    char *data;
    char **last_option_usage;
};

static int grid_has_arg (struct grid_option *opt)
{
    switch (opt->type) {
        case GRID_OPT_INCREMENT:
        case GRID_OPT_DECREMENT:
        case GRID_OPT_SET_ENUM:
        case GRID_OPT_HELP:
            return 0;
        case GRID_OPT_ENUM:
        case GRID_OPT_STRING:
        case GRID_OPT_BLOB:
        case GRID_OPT_FLOAT:
        case GRID_OPT_INT:
        case GRID_OPT_LIST_APPEND:
        case GRID_OPT_LIST_APPEND_FMT:
        case GRID_OPT_READ_FILE:
            return 1;
    }
    grid_assert (0);
}

static void grid_print_usage (struct grid_parse_context *ctx, FILE *stream)
{
    int i;
    int first;
    struct grid_option *opt;

    fprintf (stream, "    %s ", ctx->argv[0]);

    /* Print required options (long names)  */
    first = 1;
    for (i = 0;; ++i) {
        opt = &ctx->options[i];
        if (!opt->longname)
            break;
        if (opt->mask_set & ctx->requires) {
            if (first) {
                first = 0;
                fprintf (stream, "{--%s", opt->longname);
            } else {
                fprintf (stream, "|--%s", opt->longname);
            }
        }
    }
    if (!first) {
        fprintf (stream, "} ");
    }

    /* Print flag short options */
    first = 1;
    for (i = 0;; ++i) {
        opt = &ctx->options[i];
        if (!opt->longname)
            break;
        if (opt->mask_set & ctx->requires)
            continue;  /* already printed */
        if (opt->shortname && !grid_has_arg (opt)) {
            if (first) {
                first = 0;
                fprintf (stream, "[-%c", opt->shortname);
            } else {
                fprintf (stream, "%c", opt->shortname);
            }
        }
    }
    if (!first) {
        fprintf (stream, "] ");
    }

    /* Print short options with arguments */
    for (i = 0;; ++i) {
        opt = &ctx->options[i];
        if (!opt->longname)
            break;
        if (opt->mask_set & ctx->requires)
            continue;  /* already printed */
        if (opt->shortname && grid_has_arg (opt) && opt->metavar) {
            fprintf (stream, "[-%c %s] ", opt->shortname, opt->metavar);
        }
    }

    fprintf (stream, "[options] \n");  /* There may be long options too */
}

static char *grid_print_line (FILE *out, char *str, size_t width)
{
    int i;
    if (strlen (str) < width) {
        fprintf (out, "%s", str);
        return "";
    }
    for (i = width; i > 1; --i) {
        if (isspace (str[i])) {
            fprintf (out, "%.*s", i, str);
            return str + i + 1;
        }
    }  /* no break points, just print as is */
    fprintf (out, "%s", str);
    return "";
}

static void grid_print_help (struct grid_parse_context *ctx, FILE *stream)
{
    int i;
    int optlen;
    struct grid_option *opt;
    char *last_group;
    char *cursor;

    fprintf (stream, "Usage:\n");
    grid_print_usage (ctx, stream);
    fprintf (stream, "\n%s\n", ctx->def->short_description);

    last_group = NULL;
    for (i = 0;; ++i) {
        opt = &ctx->options[i];
        if (!opt->longname)
            break;
        if (!last_group || last_group != opt->group ||
            strcmp (last_group, opt->group))
        {
            fprintf (stream, "\n");
            fprintf (stream, "%s:\n", opt->group);
            last_group = opt->group;
        }
        fprintf (stream, " --%s", opt->longname);
        optlen = 3 + strlen (opt->longname);
        if (opt->shortname) {
            fprintf (stream, ",-%c", opt->shortname);
            optlen += 3;
        }
        if (grid_has_arg (opt)) {
            if (opt->metavar) {
                fprintf (stream, " %s", opt->metavar);
                optlen += strlen (opt->metavar) + 1;
            } else {
                fprintf (stream, " ARG");
                optlen += 4;
            }
        }
        if (optlen < 23) {
            fputs (&"                        "[optlen], stream);
            cursor = grid_print_line (stream, opt->description, 80-24);
        } else {
            cursor = opt->description;
        }
        while (*cursor) {
            fprintf (stream, "\n                        ");
            cursor = grid_print_line (stream, cursor, 80-24);
        }
        fprintf (stream, "\n");
    }
}

static void grid_print_option (struct grid_parse_context *ctx, int opt_index,
                            FILE *stream)
{
    char *ousage;
    char *oend;
    size_t olen;
    struct grid_option *opt;

    opt = &ctx->options[opt_index];
    ousage = ctx->last_option_usage[opt_index];
    if (*ousage == '-') {  /* Long option */
        oend = strchr (ousage, '=');
        if (!oend) {
            olen = strlen (ousage);
        } else {
            olen = (oend - ousage);
        }
        if (olen != strlen (opt->longname)+2) {
            fprintf (stream, " %.*s[%s] ",
                (int)olen, ousage, opt->longname + (olen-2));
        } else {
            fprintf (stream, " %s ", ousage);
        }
    } else if (ousage == ctx->argv[0]) {  /* Binary name */
        fprintf (stream, " %s (executable) ", ousage);
    } else {  /* Short option */
        fprintf (stream, " -%c (--%s) ",
            *ousage, opt->longname);
    }
}

static void grid_option_error (char *message, struct grid_parse_context *ctx,
                     int opt_index)
{
    fprintf (stderr, "%s: Option", ctx->argv[0]);
    grid_print_option (ctx, opt_index, stderr);
    fprintf (stderr, "%s\n", message);
    exit (1);
}


static void grid_memory_error (struct grid_parse_context *ctx) {
    fprintf (stderr, "%s: Memory error while parsing command-line",
        ctx->argv[0]);
    abort ();
}

static void grid_invalid_enum_value (struct grid_parse_context *ctx,
    int opt_index, char *argument)
{
    struct grid_option *opt;
    struct grid_enum_item *items;

    opt = &ctx->options[opt_index];
    items = (struct grid_enum_item *)opt->pointer;
    fprintf (stderr, "%s: Invalid value ``%s'' for", ctx->argv[0], argument);
    grid_print_option (ctx, opt_index, stderr);
    fprintf (stderr, ". Options are:\n");
    for (;items->name; ++items) {
        fprintf (stderr, "    %s\n", items->name);
    }
    exit (1);
}

static void grid_option_conflict (struct grid_parse_context *ctx,
                              int opt_index)
{
    unsigned long mask;
    int i;
    int num_conflicts;
    struct grid_option *opt;

    fprintf (stderr, "%s: Option", ctx->argv[0]);
    grid_print_option (ctx, opt_index, stderr);
    fprintf (stderr, "conflicts with the following options:\n");

    mask = ctx->options[opt_index].conflicts_mask;
    num_conflicts = 0;
    for (i = 0;; ++i) {
        opt = &ctx->options[i];
        if (!opt->longname)
            break;
        if (i == opt_index)
            continue;
        if (ctx->last_option_usage[i] && opt->mask_set & mask) {
            num_conflicts += 1;
            fprintf (stderr, "   ");
            grid_print_option (ctx, i, stderr);
            fprintf (stderr, "\n");
        }
    }
    if (!num_conflicts) {
        fprintf (stderr, "   ");
        grid_print_option (ctx, opt_index, stderr);
        fprintf (stderr, "\n");
    }
    exit (1);
}

static void grid_print_requires (struct grid_parse_context *ctx, unsigned long mask)
{
    int i;
    struct grid_option *opt;

    for (i = 0;; ++i) {
        opt = &ctx->options[i];
        if (!opt->longname)
            break;
        if (opt->mask_set & mask) {
            fprintf (stderr, "    --%s\n", opt->longname);
            if (opt->shortname) {
                fprintf (stderr, "    -%c\n", opt->shortname);
            }
        }
    }
    exit (1);
}

static void grid_option_requires (struct grid_parse_context *ctx, int opt_index) {
    fprintf (stderr, "%s: Option", ctx->argv[0]);
    grid_print_option (ctx, opt_index, stderr);
    fprintf (stderr, "requires at least one of the following options:\n");

    grid_print_requires (ctx, ctx->options[opt_index].requires_mask);
    exit (1);
}

static void grid_append_string (struct grid_parse_context *ctx,
                             struct grid_option *opt, char *str)
{
    struct grid_string_list *lst;

    lst = (struct grid_string_list *)(
        ((char *)ctx->target) + opt->offset);
    if (lst->items) {
        lst->num += 1;
        lst->items = realloc (lst->items, sizeof (char *) * lst->num);
    } else {
        lst->items = malloc (sizeof (char *));
        lst->num = 1;
    }
    if (!lst->items) {
        grid_memory_error (ctx);
    }
    lst->items[lst->num-1] = str;
}

static void grid_append_string_to_free (struct grid_parse_context *ctx,
                                      struct grid_option *opt, char *str)
{
    struct grid_string_list *lst;

    lst = (struct grid_string_list *)(
        ((char *)ctx->target) + opt->offset);
    if (lst->to_free) {
        lst->to_free_num += 1;
        lst->to_free = realloc (lst->items,
                                sizeof (char *) * lst->to_free_num);
    } else {
        lst->to_free = malloc (sizeof (char *));
        lst->to_free_num = 1;
    }
    if (!lst->items) {
        grid_memory_error (ctx);
    }
    lst->to_free[lst->to_free_num-1] = str;
}

static void grid_process_option (struct grid_parse_context *ctx,
                              int opt_index, char *argument)
{
    struct grid_option *opt;
    struct grid_enum_item *items;
    char *endptr;
    struct grid_blob *blob;
    FILE *file;
    char *data;
    size_t data_len;
    size_t data_buf;
    int bytes_read;

    opt = &ctx->options[opt_index];
    if (ctx->mask & opt->conflicts_mask) {
        grid_option_conflict (ctx, opt_index);
    }
    ctx->mask |= opt->mask_set;

    switch (opt->type) {
        case GRID_OPT_HELP:
            grid_print_help (ctx, stdout);
            exit (0);
            return;
        case GRID_OPT_INT:
            *(long *)(((char *)ctx->target) + opt->offset) = strtol (argument,
                &endptr, 0);
            if (endptr == argument || *endptr != 0) {
                grid_option_error ("requires integer argument",
                                ctx, opt_index);
            }
            return;
        case GRID_OPT_INCREMENT:
            *(int *)(((char *)ctx->target) + opt->offset) += 1;
            return;
        case GRID_OPT_DECREMENT:
            *(int *)(((char *)ctx->target) + opt->offset) -= 1;
            return;
        case GRID_OPT_ENUM:
            items = (struct grid_enum_item *)opt->pointer;
            for (;items->name; ++items) {
                if (!strcmp (items->name, argument)) {
                    *(int *)(((char *)ctx->target) + opt->offset) = \
                        items->value;
                    return;
                }
            }
            grid_invalid_enum_value (ctx, opt_index, argument);
            return;
        case GRID_OPT_SET_ENUM:
            *(int *)(((char *)ctx->target) + opt->offset) = \
                *(int *)(opt->pointer);
            return;
        case GRID_OPT_STRING:
            *(char **)(((char *)ctx->target) + opt->offset) = argument;
            return;
        case GRID_OPT_BLOB:
            blob = (struct grid_blob *)(((char *)ctx->target) + opt->offset);
            blob->data = argument;
            blob->length = strlen (argument);
            blob->need_free = 0;
            return;
        case GRID_OPT_FLOAT:
#if defined GRID_HAVE_WINDOWS
            *(float *)(((char *)ctx->target) + opt->offset) =
                (float) atof (argument);
#else
            *(float *)(((char *)ctx->target) + opt->offset) =
                strtof (argument, &endptr);
            if (endptr == argument || *endptr != 0) {
                grid_option_error ("requires float point argument",
                                ctx, opt_index);
            }
#endif
            return;
        case GRID_OPT_LIST_APPEND:
            grid_append_string (ctx, opt, argument);
            return;
        case GRID_OPT_LIST_APPEND_FMT:
            data_buf = strlen (argument) + strlen (opt->pointer);
            data = malloc (data_buf);
#if defined GRID_HAVE_WINDOWS
            data_len = _snprintf_s (data, data_buf, _TRUNCATE, opt->pointer,
                argument);
#else
            data_len = snprintf (data, data_buf, opt->pointer, argument);
#endif
            assert (data_len < data_buf);
            grid_append_string (ctx, opt, data);
            grid_append_string_to_free (ctx, opt, data);
            return;
        case GRID_OPT_READ_FILE:
            if (!strcmp (argument, "-")) {
                file = stdin;
            } else {
                file = fopen (argument, "r");
                if (!file) {
                    fprintf (stderr, "Error opening file ``%s'': %s\n",
                        argument, strerror (errno));
                    exit (2);
                }
            }
            data = malloc (4096);
            if (!data)
                grid_memory_error (ctx);
            data_len = 0;
            data_buf = 4096;
            for (;;) {
                bytes_read = fread (data + data_len, 1, data_buf - data_len,
                                   file);
                data_len += bytes_read;
                if (feof (file))
                    break;
                if (data_buf - data_len < 1024) {
                    if (data_buf < (1 << 20)) {
                        data_buf *= 2;  /* grow twice until not too big */
                    } else {
                        data_buf += 1 << 20;  /* grow 1 Mb each time */
                    }
                    data = realloc (data, data_buf);
                    if (!data)
                        grid_memory_error (ctx);
                }
            }
            if (data_len != data_buf) {
                data = realloc (data, data_len);
                assert (data);
            }
            if (ferror (file)) {
#if defined _MSC_VER
#pragma warning (push)
#pragma warning (disable:4996)
#endif
                fprintf (stderr, "Error reading file ``%s'': %s\n",
                    argument, strerror (errno));
#if defined _MSC_VER
#pragma warning (pop)
#endif
                exit (2);
            }
            if (file != stdin) {
                fclose (file);
            }
            blob = (struct grid_blob *)(((char *)ctx->target) + opt->offset);
            blob->data = data;
            blob->length = data_len;
            blob->need_free = 1;
            return;
    }
    abort ();
}

static void grid_parse_arg0 (struct grid_parse_context *ctx)
{
    int i;
    struct grid_option *opt;
    char *arg0;

    arg0 = strrchr (ctx->argv[0], '/');
    if (arg0 == NULL) {
        arg0 = ctx->argv[0];
    } else {
        arg0 += 1; /*  Skip slash itself  */
    }


    for (i = 0;; ++i) {
        opt = &ctx->options[i];
        if (!opt->longname)
            return;
        if (opt->arg0name && !strcmp (arg0, opt->arg0name)) {
            assert (!grid_has_arg (opt));
            ctx->last_option_usage[i] = ctx->argv[0];
            grid_process_option (ctx, i, NULL);
        }
    }
}


static void grid_error_ambiguous_option (struct grid_parse_context *ctx)
{
    struct grid_option *opt;
    char *a, *b;
    char *arg;

    arg = ctx->data+2;
    fprintf (stderr, "%s: Ambiguous option ``%s'':\n", ctx->argv[0], ctx->data);
    for (opt = ctx->options; opt->longname; ++opt) {
        for (a = opt->longname, b = arg; ; ++a, ++b) {
            if (*b == 0 || *b == '=') {  /* End of option on command-line */
                fprintf (stderr, "    %s\n", opt->longname);
                break;
            } else if (*b != *a) {
                break;
            }
        }
    }
    exit (1);
}

static void grid_error_unknown_long_option (struct grid_parse_context *ctx)
{
    fprintf (stderr, "%s: Unknown option ``%s''\n", ctx->argv[0], ctx->data);
    exit (1);
}

static void grid_error_unexpected_argument (struct grid_parse_context *ctx)
{
    fprintf (stderr, "%s: Unexpected argument ``%s''\n",
        ctx->argv[0], ctx->data);
    exit (1);
}

static void grid_error_unknown_short_option (struct grid_parse_context *ctx)
{
    fprintf (stderr, "%s: Unknown option ``-%c''\n", ctx->argv[0], *ctx->data);
    exit (1);
}

static int grid_get_arg (struct grid_parse_context *ctx)
{
    if (!ctx->args_left)
        return 0;
    ctx->args_left -= 1;
    ctx->arg += 1;
    ctx->data = *ctx->arg;
    return 1;
}

static void grid_parse_long_option (struct grid_parse_context *ctx)
{
    struct grid_option *opt;
    char *a, *b;
    int longest_prefix;
    int cur_prefix;
    int best_match;
    char *arg;
    int i;

    arg = ctx->data+2;
    longest_prefix = 0;
    best_match = -1;
    for (i = 0;; ++i) {
        opt = &ctx->options[i];
        if (!opt->longname)
            break;
        for (a = opt->longname, b = arg;; ++a, ++b) {
            if (*b == 0 || *b == '=') {  /* End of option on command-line */
                cur_prefix = a - opt->longname;
                if (!*a) {  /* Matches end of option name */
                    best_match = i;
                    longest_prefix = cur_prefix;
                    goto finish;
                }
                if (cur_prefix == longest_prefix) {
                    best_match = -1;  /* Ambiguity */
                } else if (cur_prefix > longest_prefix) {
                    best_match = i;
                    longest_prefix = cur_prefix;
                }
                break;
            } else if (*b != *a) {
                break;
            }
        }
    }
finish:
    if (best_match >= 0) {
        opt = &ctx->options[best_match];
        ctx->last_option_usage[best_match] = ctx->data;
        if (arg[longest_prefix] == '=') {
            if (grid_has_arg (opt)) {
                grid_process_option (ctx, best_match, arg + longest_prefix + 1);
            } else {
                grid_option_error ("does not accept argument", ctx, best_match);
            }
        } else {
            if (grid_has_arg (opt)) {
                if (grid_get_arg (ctx)) {
                    grid_process_option (ctx, best_match, ctx->data);
                } else {
                    grid_option_error ("requires an argument", ctx, best_match);
                }
            } else {
                grid_process_option (ctx, best_match, NULL);
            }
        }
    } else if (longest_prefix > 0) {
        grid_error_ambiguous_option (ctx);
    } else {
        grid_error_unknown_long_option (ctx);
    }
}

static void grid_parse_short_option (struct grid_parse_context *ctx)
{
    int i;
    struct grid_option *opt;

    for (i = 0;; ++i) {
        opt = &ctx->options[i];
        if (!opt->longname)
            break;
        if (!opt->shortname)
            continue;
        if (opt->shortname == *ctx->data) {
            ctx->last_option_usage[i] = ctx->data;
            if (grid_has_arg (opt)) {
                if (ctx->data[1]) {
                    grid_process_option (ctx, i, ctx->data+1);
                } else {
                    if (grid_get_arg (ctx)) {
                        grid_process_option (ctx, i, ctx->data);
                    } else {
                        grid_option_error ("requires an argument", ctx, i);
                    }
                }
                ctx->data = "";  /* end of short options anyway */
            } else {
                grid_process_option (ctx, i, NULL);
                ctx->data += 1;
            }
            return;
        }
    }
    grid_error_unknown_short_option (ctx);
}


static void grid_parse_arg (struct grid_parse_context *ctx)
{
    if (ctx->data[0] == '-') {  /* an option */
        if (ctx->data[1] == '-') {  /* long option */
            if (ctx->data[2] == 0) {  /* end of options */
                return;
            }
            grid_parse_long_option (ctx);
        } else {
            ctx->data += 1;  /* Skip minus */
            while (*ctx->data) {
                grid_parse_short_option (ctx);
            }
        }
    } else {
        grid_error_unexpected_argument (ctx);
    }
}

void grid_check_requires (struct grid_parse_context *ctx) {
    int i;
    struct grid_option *opt;

    for (i = 0;; ++i) {
        opt = &ctx->options[i];
        if (!opt->longname)
            break;
        if (!ctx->last_option_usage[i])
            continue;
        if (opt->requires_mask &&
            (opt->requires_mask & ctx->mask) != opt->requires_mask) {
            grid_option_requires (ctx, i);
        }
    }

    if ((ctx->requires & ctx->mask) != ctx->requires) {
        fprintf (stderr, "%s: At least one of the following required:\n",
            ctx->argv[0]);
        grid_print_requires (ctx, ctx->requires & ~ctx->mask);
        exit (1);
    }
}

void grid_parse_options (struct grid_commandline *cline,
    void *target, int argc, char **argv)
{
    struct grid_parse_context ctx;
    int num_options;

    ctx.def = cline;
    ctx.options = cline->options;
    ctx.target = target;
    ctx.argc = argc;
    ctx.argv = argv;
    ctx.requires = cline->required_options;

    for (num_options = 0; ctx.options[num_options].longname; ++num_options);
    ctx.last_option_usage = calloc (sizeof (char *), num_options);
    if  (!ctx.last_option_usage)
        grid_memory_error (&ctx);

    ctx.mask = 0;
    ctx.args_left = argc - 1;
    ctx.arg = argv;

    grid_parse_arg0 (&ctx);

    while (grid_get_arg (&ctx)) {
        grid_parse_arg (&ctx);
    }

    grid_check_requires (&ctx);

    free (ctx.last_option_usage);

}

void grid_free_options (struct grid_commandline *cline, void *target) {
    int i, j;
    struct grid_option *opt;
    struct grid_blob *blob;
    struct grid_string_list *lst;

    for (i = 0;; ++i) {
        opt = &cline->options[i];
        if (!opt->longname)
            break;
        switch(opt->type) {
        case GRID_OPT_LIST_APPEND:
        case GRID_OPT_LIST_APPEND_FMT:
            lst = (struct grid_string_list *)(((char *)target) + opt->offset);
            if(lst->items) {
                free(lst->items);
                lst->items = NULL;
            }
            if(lst->to_free) {
                for(j = 0; j < lst->to_free_num; ++j) {
                    free(lst->to_free[j]);
                }
                free(lst->to_free);
                lst->to_free = NULL;
            }
            break;
        case GRID_OPT_READ_FILE:
        case GRID_OPT_BLOB:
            blob = (struct grid_blob *)(((char *)target) + opt->offset);
            if(blob->need_free && blob->data) {
                free(blob->data);
                blob->need_free = 0;
            }
            break;
        default:
            break;
        }
    }
}

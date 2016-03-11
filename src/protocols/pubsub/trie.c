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

#include <string.h>
#include <assert.h>
#include <stdio.h>

#include "trie.h"
#include "../../utils/alloc.h"
#include "../../utils/fast.h"
#include "../../utils/err.h"

/*  Double check that the size of node structure is as small as
    we believe it to be. */
CT_ASSERT (sizeof (struct grid_trie_node) == 24);

/*  Forward declarations. */
static struct grid_trie_node *grid_node_compact (struct grid_trie_node *self);
static int grid_node_check_prefix (struct grid_trie_node *self,
    const uint8_t *data, size_t size);
static struct grid_trie_node **grid_node_child (struct grid_trie_node *self,
    int index);
static struct grid_trie_node **grid_node_next (struct grid_trie_node *self,
    uint8_t c);
static int grid_node_unsubscribe (struct grid_trie_node **self,
    const uint8_t *data, size_t size);
static void grid_node_term (struct grid_trie_node *self);
static int grid_node_has_subscribers (struct grid_trie_node *self);
static void grid_node_dump (struct grid_trie_node *self, int indent);
static void grid_node_indent (int indent);
static void grid_node_putchar (uint8_t c);

void grid_trie_init (struct grid_trie *self)
{
    self->root = NULL;
}

void grid_trie_term (struct grid_trie *self)
{
    grid_node_term (self->root);
}

void grid_trie_dump (struct grid_trie *self)
{
    grid_node_dump (self->root, 0);
}

void grid_node_dump (struct grid_trie_node *self, int indent)
{
    int i;
    int children;

    if (!self) {
        grid_node_indent (indent);
        printf ("NULL\n");
        return;
    }

    grid_node_indent (indent);
    printf ("===================\n");
    grid_node_indent (indent);
    printf ("refcount=%d\n", (int) self->refcount);
    grid_node_indent (indent);
    printf ("prefix_len=%d\n", (int) self->prefix_len);
    grid_node_indent (indent);
    if (self->type == GRID_TRIE_DENSE_TYPE)
        printf ("type=dense\n");
    else
        printf ("type=sparse\n");
    grid_node_indent (indent);
    printf ("prefix=\"");
    for (i = 0; i != self->prefix_len; ++i)
        grid_node_putchar (self->prefix [i]);
    printf ("\"\n");
    if (self->type <= 8) {
        grid_node_indent (indent);
        printf ("sparse.children=\"");
        for (i = 0; i != self->type; ++i)
            grid_node_putchar (self->u.sparse.children [i]);
        printf ("\"\n");
        children = self->type;
    }
    else {
        grid_node_indent (indent);
        printf ("dense.min='%c' (%d)\n", (char) self->u.dense.min,
            (int) self->u.dense.min);
        grid_node_indent (indent);
        printf ("dense.max='%c' (%d)\n", (char) self->u.dense.max,
            (int) self->u.dense.max);
        grid_node_indent (indent);
        printf ("dense.nbr=%d\n", (int) self->u.dense.nbr);
        children = self->u.dense.max - self->u.dense.min + 1;
    }

    for (i = 0; i != children; ++i)
        grid_node_dump (((struct grid_trie_node**) (self + 1)) [i], indent + 1);

    grid_node_indent (indent);
    printf ("===================\n");
}

void grid_node_indent (int indent)
{
    int i;

    for (i = 0; i != indent * 4; ++i)
        grid_node_putchar (' ');
}

void grid_node_putchar (uint8_t c)
{
    if (c < 32 || c > 127)
        putchar ('?');
    else
        putchar (c);
}

void grid_node_term (struct grid_trie_node *self)
{
    int children;
    int i;

    /*  Trivial case of the recursive algorithm. */
    if (!self)
        return;

    /*  Recursively destroy the child nodes. */
    children = self->type <= GRID_TRIE_SPARSE_MAX ?
        self->type : (self->u.dense.max - self->u.dense.min + 1);
    for (i = 0; i != children; ++i)
        grid_node_term (*grid_node_child (self, i));

    /*  Deallocate this node. */
    grid_free (self);
}

int grid_node_check_prefix (struct grid_trie_node *self,
    const uint8_t *data, size_t size)
{
    /*  Check how many characters from the data match the prefix. */

    int i;

    for (i = 0; i != self->prefix_len; ++i) {
        if (!size || self->prefix [i] != *data)
            return i;
        ++data;
        --size;
    }
    return self->prefix_len;
}

struct grid_trie_node **grid_node_child (struct grid_trie_node *self, int index)
{
    /*  Finds pointer to the n-th child of the node. */

    return ((struct grid_trie_node**) (self + 1)) + index;
}

struct grid_trie_node **grid_node_next (struct grid_trie_node *self, uint8_t c)
{
    /*  Finds the pointer to the next node based on the supplied character.
        If there is no such pointer, it returns NULL. */

    int i;

    if (self->type == 0)
        return NULL;

    /*  Sparse mode. */
    if (self->type <= 8) {
        for (i = 0; i != self->type; ++i)
            if (self->u.sparse.children [i] == c)
                return grid_node_child (self, i);
        return NULL;
    }

    /*  Dense mode. */
    if (c < self->u.dense.min || c > self->u.dense.max)
        return NULL;
    return grid_node_child (self, c - self->u.dense.min);
}

struct grid_trie_node *grid_node_compact (struct grid_trie_node *self)
{
    /*  Tries to merge the node with the child node. Returns pointer to
        the compacted node. */

    struct grid_trie_node *ch;

    /*  Node that is a subscription cannot be compacted. */
    if (grid_node_has_subscribers (self))
        return self;

    /*  Only a node with a single child can be compacted. */
    if (self->type != 1)
        return self;

    /*  Check whether combined prefixes would fix into a single node. */
    ch = *grid_node_child (self, 0);
    if (self->prefix_len + ch->prefix_len + 1 > GRID_TRIE_PREFIX_MAX)
        return self;

    /*  Concatenate the prefixes. */
    memmove (ch->prefix + self->prefix_len + 1, ch->prefix, ch->prefix_len);
    memcpy (ch->prefix, self->prefix, self->prefix_len);
    ch->prefix [self->prefix_len] = self->u.sparse.children [0];
    ch->prefix_len += self->prefix_len + 1;

    /*  Get rid of the obsolete parent node. */
    grid_free (self);

    /*  Return the new compacted node. */
    return ch;
}

int grid_trie_subscribe (struct grid_trie *self, const uint8_t *data, size_t size)
{
    int i;
    struct grid_trie_node **node;
    struct grid_trie_node **n;
    struct grid_trie_node *ch;
    struct grid_trie_node *old_node;
    int pos;
    uint8_t c;
    uint8_t c2;
    uint8_t new_min;
    uint8_t new_max;
    int old_children;
    int new_children;
    int inserted;
    int more_nodes;

    /*  Step 1 -- Traverse the trie. */

    node = &self->root;
    pos = 0;
    while (1) {

        /*  If there are no more nodes on the path, go to step 4. */
        if (!*node)
            goto step4;

        /*  Check whether prefix matches the new subscription. */
        pos = grid_node_check_prefix (*node, data, size);
        data += pos;
        size -= pos;

        /*  If only part of the prefix matches, go to step 2. */
        if (pos < (*node)->prefix_len)
            goto step2;

        /*  Even if whole prefix matches and there's no more data to match,
            go directly to step 5. */
        if (!size)
            goto step5;

        /*  Move to the next node. If it is not present, go to step 3. */
        n = grid_node_next (*node, *data);
        if (!n || !*n)
            goto step3;
        node = n;
        ++data;
        --size;
    }

    /*  Step 2 -- Split the prefix into two parts if required. */
step2:

    ch = *node;
    *node = grid_alloc (sizeof (struct grid_trie_node) +
        sizeof (struct grid_trie_node*), "trie node");
    assert (*node);
    (*node)->refcount = 0;
    (*node)->prefix_len = pos;
    (*node)->type = 1;
    memcpy ((*node)->prefix, ch->prefix, pos);
    (*node)->u.sparse.children [0] = ch->prefix [pos];
    ch->prefix_len -= (pos + 1);
    memmove (ch->prefix, ch->prefix + pos + 1, ch->prefix_len);
    ch = grid_node_compact (ch);
    *grid_node_child (*node, 0) = ch;
    pos = (*node)->prefix_len;

    /*  Step 3 -- Adjust the child array to accommodate the new character. */
step3:

    /*  If there are no more data in the subscription, there's nothing to
        adjust in the child array. Proceed directly to the step 5. */
    if (!size)
        goto step5;

    /*  If the new branch fits into sparse array... */
    if ((*node)->type < GRID_TRIE_SPARSE_MAX) {
        *node = grid_realloc (*node, sizeof (struct grid_trie_node) +
            ((*node)->type + 1) * sizeof (struct grid_trie_node*));
        assert (*node);
        (*node)->u.sparse.children [(*node)->type] = *data;
        ++(*node)->type;
        node = grid_node_child (*node, (*node)->type - 1);
        *node = NULL;
        ++data;
        --size;
        goto step4;
    }

    /*  If the node is already a dense array, resize it to fit the next
        character. */
    if ((*node)->type == GRID_TRIE_DENSE_TYPE) {
        c = *data;
        if (c < (*node)->u.dense.min || c > (*node)->u.dense.max) {
            new_min = (*node)->u.dense.min < c ? (*node)->u.dense.min : c;
            new_max = (*node)->u.dense.max > c ? (*node)->u.dense.max : c;
            *node = grid_realloc (*node, sizeof (struct grid_trie_node) +
                (new_max - new_min + 1) * sizeof (struct grid_trie_node*));
            assert (*node);
            old_children = (*node)->u.dense.max - (*node)->u.dense.min + 1;
            new_children = new_max - new_min + 1;
            if ((*node)->u.dense.min != new_min) {
                inserted = (*node)->u.dense.min - new_min;
                memmove (grid_node_child (*node, inserted),
                    grid_node_child (*node, 0),
                    old_children * sizeof (struct grid_trie_node*));
                memset (grid_node_child (*node, 0), 0,
                    inserted * sizeof (struct grid_trie_node*));
            }
            else {
                memset (grid_node_child (*node, old_children), 0,
                    (new_children - old_children) *
                    sizeof (struct grid_trie_node*));
            }
            (*node)->u.dense.min = new_min;
            (*node)->u.dense.max = new_max;
        }
        ++(*node)->u.dense.nbr;

        node = grid_node_child (*node, c - (*node)->u.dense.min);
        ++data;
        --size;
        goto step4;
    }

    /*  This is a sparse array, but no more children can be added to it.
        We have to convert it into a dense array. */
    {
        /*  First, determine the range of children. */
        new_min = 255;
        new_max = 0;
        for (i = 0; i != (*node)->type; ++i) {
            c2 = (*node)->u.sparse.children [i];
            new_min = new_min < c2 ? new_min : c2;
            new_max = new_max > c2 ? new_max : c2;
        }
        new_min = new_min < *data ? new_min : *data;
        new_max = new_max > *data ? new_max : *data;

        /*  Create a new mode, while keeping the old one for a while. */
        old_node = *node;
        *node = (struct grid_trie_node*) grid_alloc (sizeof (struct grid_trie_node) +
            (new_max - new_min + 1) * sizeof (struct grid_trie_node*),
            "trie node");
        assert (*node);

        /*  Fill in the new node. */
        (*node)->refcount = 0;
        (*node)->prefix_len = old_node->prefix_len;
        (*node)->type = GRID_TRIE_DENSE_TYPE;
        memcpy ((*node)->prefix, old_node->prefix, old_node->prefix_len);
        (*node)->u.dense.min = new_min;
        (*node)->u.dense.max = new_max;
        (*node)->u.dense.nbr = old_node->type + 1;
        memset (*node + 1, 0, (new_max - new_min + 1) *
            sizeof (struct grid_trie_node*));
        for (i = 0; i != old_node->type; ++i)
            *grid_node_child (*node, old_node->u.sparse.children [i] - new_min) =
                *grid_node_child (old_node, i);
        node = grid_node_next (*node, *data);
        ++data;
        --size;

        /*  Get rid of the obsolete old node. */
        grid_free (old_node);
    }

    /*  Step 4 -- Create new nodes for remaining part of the subscription. */
step4:

    assert (!*node);
    while (1) {

        /*  Create a new node to hold the next part of the subscription. */
        more_nodes = size > GRID_TRIE_PREFIX_MAX;
        *node = grid_alloc (sizeof (struct grid_trie_node) +
            (more_nodes ? sizeof (struct grid_trie_node*) : 0), "trie node");
        assert (*node);

        /*  Fill in the new node. */
        (*node)->refcount = 0;
        (*node)->type = more_nodes ? 1 : 0;
        (*node)->prefix_len = size < (uint8_t) GRID_TRIE_PREFIX_MAX ?
            (uint8_t) size : (uint8_t) GRID_TRIE_PREFIX_MAX;
        memcpy ((*node)->prefix, data, (*node)->prefix_len);
        data += (*node)->prefix_len;
        size -= (*node)->prefix_len;
        if (!more_nodes)
            break;
        (*node)->u.sparse.children [0] = *data;
        node = grid_node_child (*node, 0);
        ++data;
        --size;
    }

    /*  Step 5 -- Create the subscription as such. */
step5:

    ++(*node)->refcount;

    /*  Return 1 in case of a fresh subscription. */
    return (*node)->refcount == 1 ? 1 : 0;
}

int grid_trie_match (struct grid_trie *self, const uint8_t *data, size_t size)
{
    struct grid_trie_node *node;
    struct grid_trie_node **tmp;

    node = self->root;
    while (1) {

        /*  If we are at the end of the trie, return. */
        if (!node)
            return 0;

        /*  Check whether whole prefix matches the data. If not so,
            the whole string won't match. */
        if (grid_node_check_prefix (node, data, size) != node->prefix_len)
            return 0;

        /*  Skip the prefix. */
        data += node->prefix_len;
        size -= node->prefix_len;

        /*  If all the data are matched, return. */
        if (grid_node_has_subscribers (node))
            return 1;

        /*  Move to the next node. */
        tmp = grid_node_next (node, *data);
        node = tmp ? *tmp : NULL;
        ++data;
        --size;
    }
}

int grid_trie_unsubscribe (struct grid_trie *self, const uint8_t *data, size_t size)
{
    return grid_node_unsubscribe (&self->root, data, size);
}

static int grid_node_unsubscribe (struct grid_trie_node **self,
    const uint8_t *data, size_t size)
{
    int i;
    int j;
    int index;
    int new_min;
    struct grid_trie_node **ch;
    struct grid_trie_node *new_node;
    struct grid_trie_node *ch2;

    if (!size)
        goto found;

    /*  If prefix does not match the data, return. */
    if (grid_node_check_prefix (*self, data, size) != (*self)->prefix_len)
        return 0;

    /*  Skip the prefix. */
    data += (*self)->prefix_len;
    size -= (*self)->prefix_len;

    if (!size)
        goto found;

    /*  Move to the next node. */
    ch = grid_node_next (*self, *data);
    if (!ch)
        return 0; /*  TODO: This should be an error. */

    /*  Recursive traversal of the trie happens here. If the subscription
        wasn't really removed, nothing have changed in the trie and
        no additional pruning is needed. */
    if (grid_node_unsubscribe (ch, data + 1, size - 1) == 0)
        return 0;

    /*  Subscription removal is already done. Now we are going to compact
        the trie. However, if the following node remains in place, there's
        nothing to compact here. */
    if (*ch)
        return 1;

    /*  Sparse array. */
    if ((*self)->type < GRID_TRIE_DENSE_TYPE) {

        /*  Get the indices of the removed child. */
        for (index = 0; index != (*self)->type; ++index)
            if ((*self)->u.sparse.children [index] == *data)
                break;
        assert (index != (*self)->type);

        /*  Remove the destroyed child from both lists of children. */
        memmove (
            (*self)->u.sparse.children + index,
            (*self)->u.sparse.children + index + 1,
            (*self)->type - index - 1);
        memmove (
            grid_node_child (*self, index),
            grid_node_child (*self, index + 1),
            ((*self)->type - index - 1) * sizeof (struct grid_trie_node*));
        --(*self)->type;
        *self = grid_realloc (*self, sizeof (struct grid_trie_node) +
            ((*self)->type * sizeof (struct grid_trie_node*)));
        assert (*self);
        
        /*  If there are no more children and no refcount, we can delete
            the node altogether. */
        if (!(*self)->type && !grid_node_has_subscribers (*self)) {
            grid_free (*self);
            *self = NULL;
            return 1;
        }

        /*  Try to merge the node with the following node. */
        *self = grid_node_compact (*self);

        return 1;
    }

    /*  Dense array. */

    /*  In this case the array stays dense. We have to adjust the limits of
        the array, if appropriate. */
    if ((*self)->u.dense.nbr > GRID_TRIE_SPARSE_MAX + 1) {

        /*  If the removed item is the leftmost one, trim the array from
            the left side. */
        if (*data == (*self)->u.dense.min) {
             for (i = 0; i != (*self)->u.dense.max - (*self)->u.dense.min + 1;
                   ++i)
                 if (*grid_node_child (*self, i))
                     break;
             new_min = i + (*self)->u.dense.min;
             memmove (grid_node_child (*self, 0), grid_node_child (*self, i),
                 ((*self)->u.dense.max - new_min + 1) *
                 sizeof (struct grid_trie_node*));
             (*self)->u.dense.min = new_min;
             --(*self)->u.dense.nbr;
             *self = grid_realloc (*self, sizeof (struct grid_trie_node) +
                 ((*self)->u.dense.max - new_min + 1) *
                 sizeof (struct grid_trie_node*));
             assert (*self);
             return 1;
        }

        /*  If the removed item is the rightmost one, trim the array from
            the right side. */
        if (*data == (*self)->u.dense.max) {
             for (i = (*self)->u.dense.max - (*self)->u.dense.min; i != 0; --i)
                 if (*grid_node_child (*self, i))
                     break;
             (*self)->u.dense.max = i + (*self)->u.dense.min;
             --(*self)->u.dense.nbr;
             *self = grid_realloc (*self, sizeof (struct grid_trie_node) +
                 ((*self)->u.dense.max - (*self)->u.dense.min + 1) *
                 sizeof (struct grid_trie_node*));
             assert (*self);
             return 1;
        }

        /*  If the item is removed from the middle of the array, do nothing. */
        --(*self)->u.dense.nbr;
        return 1;
    }

    /*  Convert dense array into sparse array. */
    {
        new_node = grid_alloc (sizeof (struct grid_trie_node) +
            GRID_TRIE_SPARSE_MAX * sizeof (struct grid_trie_node*), "trie node");
        assert (new_node);
        new_node->refcount = 0;
        new_node->prefix_len = (*self)->prefix_len;
        memcpy (new_node->prefix, (*self)->prefix, new_node->prefix_len);
        new_node->type = GRID_TRIE_SPARSE_MAX;
        j = 0;
        for (i = 0; i != (*self)->u.dense.max - (*self)->u.dense.min + 1;
              ++i) {
            ch2 = *grid_node_child (*self, i);
            if (ch2) {
                new_node->u.sparse.children [j] = i + (*self)->u.dense.min;
                *grid_node_child (new_node, j) = ch2;
                ++j;
            }
        }
        assert (j == GRID_TRIE_SPARSE_MAX);
        grid_free (*self);
        *self = new_node;
        return 1;
    }

found:

    /*  We are at the end of the subscription here. */

    /*  Subscription doesn't exist. */
    if (grid_slow (!*self || !grid_node_has_subscribers (*self)))
        return -EINVAL;

    /*  Subscription exists. Unsubscribe. */
    --(*self)->refcount;

    /*  If reference count has dropped to zero we can try to compact
        the node. */
    if (!(*self)->refcount) {

        /*  If there are no children, we can delete the node altogether. */
        if (!(*self)->type) {
            grid_free (*self);
            *self = NULL;
            return 1;
        }

        /*  Try to merge the node with the following node. */
        *self = grid_node_compact (*self);
        return 1;
    }

    return 0;
}

int grid_node_has_subscribers (struct grid_trie_node *node)
{
    /*  Returns 1 when there are no subscribers associated with the node. */
    return node->refcount ? 1 : 0;
}


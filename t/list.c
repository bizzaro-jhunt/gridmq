/*
    Copyright (c) 2013 Nir Soffer <nirsof@gmail.com>

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

#include "../src/utils/cont.h"

#include "../src/utils/err.c"
#include "../src/utils/list.c"

static struct grid_list_item sentinel;

/*  Typical object that can be added to a list. */
struct item {
    int value;
    struct grid_list_item item;
};

/*  Initializing list items statically so they can be inserted into a list. */
static struct item that = {1, GRID_LIST_ITEM_INITIALIZER};
static struct item other = {2, GRID_LIST_ITEM_INITIALIZER};

int main ()
{
    int rc;
    struct grid_list list;
    struct grid_list_item *list_item;
    struct item *item;

    /*  List item life cycle. */

    /*  Initialize the item. Make sure it's not part of any list. */
    grid_list_item_init (&that.item);
    grid_assert (!grid_list_item_isinlist (&that.item));

    /*  That may be part of some list, or uninitialized memory. */
    that.item.prev = &sentinel;
    that.item.next = &sentinel;
    grid_assert (grid_list_item_isinlist (&that.item));
    that.item.prev = NULL;
    that.item.next = NULL;
    grid_assert (grid_list_item_isinlist (&that.item));

    /*  Before termination, item must be removed from the list. */
    grid_list_item_init (&that.item);
    grid_list_item_term (&that.item);

    /*  Initializing a list. */

    /*  Uninitialized list has random content. */
    list.first = &sentinel;
    list.last = &sentinel;

    grid_list_init (&list);

    grid_assert (list.first == NULL);
    grid_assert (list.last == NULL);

    grid_list_term (&list);
    
    /*  Empty list. */
    
    grid_list_init (&list);

    rc = grid_list_empty (&list);
    grid_assert (rc == 1); 

    list_item = grid_list_begin (&list);
    grid_assert (list_item == NULL);

    list_item = grid_list_end (&list);
    grid_assert (list_item == NULL);

    grid_list_term (&list);

    /*  Inserting and erasing items. */

    grid_list_init (&list);
    grid_list_item_init (&that.item);

    /*  Item doesn'tt belong to list yet. */
    grid_assert (!grid_list_item_isinlist (&that.item));

    grid_list_insert (&list, &that.item, grid_list_end (&list));

    /*  Item is now part of a list. */
    grid_assert (grid_list_item_isinlist (&that.item));

    /*  Single item does not have prev or next item. */
    grid_assert (that.item.prev == NULL);
    grid_assert (that.item.next == NULL);

    /*  Item is both first and list item. */
    grid_assert (list.first == &that.item);
    grid_assert (list.last == &that.item);

    /*  Removing an item. */
    grid_list_erase (&list, &that.item);
    grid_assert (!grid_list_item_isinlist (&that.item));

    grid_assert (list.first == NULL);
    grid_assert (list.last == NULL);

    grid_list_item_term (&that.item);
    grid_list_term (&list);

    /*  Iterating items. */
    
    grid_list_init (&list);
    grid_list_item_init (&that.item);

    grid_list_insert (&list, &that.item, grid_list_end (&list));
    
    list_item = grid_list_begin (&list);
    grid_assert (list_item == &that.item);

    item = grid_cont (list_item, struct item, item);
    grid_assert (item == &that);

    list_item = grid_list_end (&list);
    grid_assert (list_item == NULL);

    list_item = grid_list_prev (&list, &that.item);
    grid_assert (list_item == NULL);

    list_item = grid_list_next (&list, &that.item);
    grid_assert (list_item == NULL);

    rc = grid_list_empty (&list);
    grid_assert (rc == 0);

    grid_list_erase (&list, &that.item);
    grid_list_item_term (&that.item);
    grid_list_term (&list);

    /*  Appending items. */

    grid_list_init (&list);
    grid_list_item_init (&that.item);
    grid_list_item_init (&other.item);

    grid_list_insert (&list, &that.item, grid_list_end (&list));
    grid_list_insert (&list, &other.item, grid_list_end (&list));

    list_item = grid_list_begin (&list);
    grid_assert (list_item == &that.item);

    list_item = grid_list_next (&list, list_item);
    grid_assert (list_item == &other.item);

    grid_list_erase (&list, &that.item);
    grid_list_erase (&list, &other.item);
    grid_list_item_term (&that.item);
    grid_list_item_term (&other.item);
    grid_list_term (&list);

    /*  Prepending items. */

    grid_list_init (&list);
    grid_list_item_init (&that.item);
    grid_list_item_init (&other.item);

    grid_list_insert (&list, &that.item, grid_list_begin (&list));
    grid_list_insert (&list, &other.item, grid_list_begin (&list));

    list_item = grid_list_begin (&list);
    grid_assert (list_item == &other.item);

    list_item = grid_list_next (&list, list_item);
    grid_assert (list_item == &that.item);

    grid_list_erase (&list, &that.item);
    grid_list_erase (&list, &other.item);
    grid_list_item_term (&that.item);
    grid_list_item_term (&other.item);
    grid_list_term (&list);

    return 0;
}


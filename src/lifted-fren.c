/* The index openbst looks a build up in, cut down to the five builds this
 * module offers: the French and English ones. It stands in for openbst's
 * src/data/lifted.c so that only those five sets of tables are linked. */

#include <string.h>
#include "bst_text.h"

extern const bst_lifted BST_DATA_1995;
extern const bst_lifted BST_DATA_1998ENG;
extern const bst_lifted BST_DATA_1998FRN;
extern const bst_lifted BST_DATA_2006ENG;
extern const bst_lifted BST_DATA_2006FRE;

static const struct { const char *name; const bst_lifted *d; } INDEX[] = {
    { "1995", &BST_DATA_1995 },
    { "1998ENG", &BST_DATA_1998ENG },
    { "1998FRN", &BST_DATA_1998FRN },
    { "2006ENG", &BST_DATA_2006ENG },
    { "2006FRE", &BST_DATA_2006FRE },
};

const bst_lifted *bst_lifted_for(const char *build) {
    if (!build) return NULL;
    for (size_t i = 0; i < sizeof INDEX / sizeof INDEX[0]; i++)
        if (strcmp(INDEX[i].name, build) == 0) return INDEX[i].d;
    return NULL;
}

/* The switches of the picture mode screen's column (E148 1.100; pictprof.c, "The look's optics rows"): n entries
 * {u32 key, u32 target}, n a power of two; a key (the item - 6) sits at index key & (n - 1) or at the first free index
 * after it (linear probing, wrapping); key 0 is an empty entry, where a lookup of an absent key ends. Plain C over the
 * table's words, also run by the host tests (tests/host/test_rowsw.c) */
#ifndef ROWSW_H
#define ROWSW_H

#include "omk_types.h"

/* The index + 1 of key's entry, 0 - absent */
static u32 sw_find(volatile const u32 *tab, u32 n, u32 key)
{
    u32 i, k;

    for (i = 0; i < n; i++) {
        k = (key + i) & (n - 1);
        if (tab[2 * k] == key)
            return k + 1;
        if (!tab[2 * k])
            return 0;
    }
    return 0;
}

/* key with target: its entry's target changed, or the first free entry of its chain taken - the target first, then the
 * key, so a lookup never finds the key with another target - while one free entry is left after it (every lookup of an
 * absent key ends). 0 in place, 1 put, -1 no room */
static int sw_put(volatile u32 *tab, u32 n, u32 key, u32 target)
{
    u32 i, k, e = sw_find(tab, n, key), free = 0;

    if (e) {
        if (tab[2 * (e - 1) + 1] == target)
            return 0;
        tab[2 * (e - 1) + 1] = target;
        return 1;
    }
    for (i = 0; i < n; i++)
        free += !tab[2 * i];
    if (free < 2)
        return -1;
    for (i = 0; i < n; i++) {
        k = (key + i) & (n - 1);
        if (!tab[2 * k]) {
            tab[2 * k + 1] = target;
            tab[2 * k] = key;
            return 1;
        }
    }
    return -1;
}

#endif

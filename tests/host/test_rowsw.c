/* Host test of the column's switches (src/platform/om5m2/fw1100/rowsw.h): a key lands where the firmware's lookup finds
 * it - its index, or past the taken ones with the wrap - and a lookup stops at the first free entry; the target goes in
 * before the key; one entry always stays free; a key in place gets its target changed, not a second entry. The tables of
 * E148 1.100 the optics rows use: 32 entries with 15 free, 16 entries with 3 free. Prints "ok" or the failed checks */
#include <stdio.h>
#include <string.h>

#include "rowsw.h"

static int failed;

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);             \
            failed++;                                                           \
        }                                                                       \
    } while (0)

/* The firmware's lookup as at 0x60e4a206..0x60e4a237: from key & (n - 1) on, wrapping, until the key or a free entry */
static u32 fw_lookup(const u32 *tab, u32 n, u32 key)
{
    u32 i = key & (n - 1), k;

    for (k = 0; k < n; k++, i = (i + 1) & (n - 1)) {
        if (tab[2 * i] == key)
            return tab[2 * i + 1];
        if (!tab[2 * i])
            return 0;
    }
    return 0;
}

static u32 free_of(const u32 *tab, u32 n)
{
    u32 i, f = 0;

    for (i = 0; i < n; i++)
        f += !tab[2 * i];
    return f;
}

int main(void)
{
    static const u32 keys32[17] = {1, 2, 53, 99, 105, 124, 125, 210, 300, 861, 5023, 5367, 5368, 5369, 5371, 5364, 3};
    static const u32 keys16[13] = {1, 2, 53, 99, 105, 124, 125, 210, 300, 5367, 5368, 5369, 5023};
    u32 t32[64], t16[32], i;

    /* 32 entries, 17 stock keys (15 free): our two keys 5366 and 5364 (items 5372 and 5370) - 5364 is taken here by a
     * stock key on purpose (its target changes, no second entry) */
    memset(t32, 0, sizeof(t32));
    for (i = 0; i < 17; i++)
        CHECK(sw_put(t32, 32, keys32[i], 0x1000 + i) == 1);
    CHECK(free_of(t32, 32) == 15);
    for (i = 0; i < 17; i++)
        CHECK(fw_lookup(t32, 32, keys32[i]) == 0x1000 + i && sw_find(t32, 32, keys32[i]));
    CHECK(sw_put(t32, 32, 5366, 0xa) == 1 && fw_lookup(t32, 32, 5366) == 0xa && free_of(t32, 32) == 14);
    CHECK(sw_put(t32, 32, 5366, 0xa) == 0 && free_of(t32, 32) == 14);   /* in place */
    CHECK(sw_put(t32, 32, 5366, 0xb) == 1 && fw_lookup(t32, 32, 5366) == 0xb && free_of(t32, 32) == 14);
    CHECK(sw_put(t32, 32, 5364, 0xc) == 1 && fw_lookup(t32, 32, 5364) == 0xc && free_of(t32, 32) == 14);
    for (i = 0; i < 17; i++)                /* every stock key still found, with its own target (but 5364's) */
        CHECK(fw_lookup(t32, 32, keys32[i]) == (keys32[i] == 5364 ? 0xc : 0x1000 + i));
    CHECK(!fw_lookup(t32, 32, 4) && !sw_find(t32, 32, 4));   /* an absent key: not found, the lookup ends */

    /* 16 entries, 13 stock keys (3 free): two keys go in, the third would leave none free - refused */
    memset(t16, 0, sizeof(t16));
    for (i = 0; i < 13; i++)
        CHECK(sw_put(t16, 16, keys16[i], 0x2000 + i) == 1);
    CHECK(free_of(t16, 16) == 3);
    CHECK(sw_put(t16, 16, 5366, 0xa) == 1 && sw_put(t16, 16, 5364, 0xb) == 1 && free_of(t16, 16) == 1);
    CHECK(fw_lookup(t16, 16, 5366) == 0xa && fw_lookup(t16, 16, 5364) == 0xb);
    CHECK(sw_put(t16, 16, 7, 0xc) == -1 && free_of(t16, 16) == 1 && !fw_lookup(t16, 16, 7));
    for (i = 0; i < 13; i++)
        CHECK(fw_lookup(t16, 16, keys16[i]) == 0x2000 + i);

    /* the wrap: a key whose chain runs past the last entry lands at the start */
    memset(t16, 0, sizeof(t16));
    CHECK(sw_put(t16, 16, 15, 1) == 1 && sw_put(t16, 16, 31, 2) == 1 && t16[0] == 31 && t16[1] == 2);
    CHECK(fw_lookup(t16, 16, 31) == 2 && fw_lookup(t16, 16, 15) == 1);
    if (failed)
        return 1;
    printf("ok\n");
    return 0;
}

/* Host test of the grain texture's fill (src/platform/om5m2/fw1100/texfill.h, the code the driver's hook runs): the fill by
 * rows through a row buffer gives, byte for byte, the texture of the word-by-word fill, and both are the definition -
 * byte (x, y) of the result is byte ((x + dx) mod side, (y + dy) mod side) of the tile. Sides from the smallest to the
 * whole texture, shifts at their ends and in between. Prints "ok", or the cases that differ. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "omk_types.h"
#include "texfill.h"

#define T TEX_FILL_TILE

static u32 tile[T * T / 4], a[T * T / 4], b[T * T / 4], row[TEX_FILL_ROW];
static u32 seed = 0x2545f491u;

static u32 rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static int one(u32 side, u32 dx, u32 dy)
{
    const u8 *t8 = (const u8 *)tile, *a8 = (const u8 *)a;
    u32 i, x, y;

    memset(a, 0xa5, sizeof(a));
    memset(b, 0x5a, sizeof(b));
    memset(row, 0xff, sizeof(row));
    tex_fill_direct(a, tile, side, dx, dy);
    tex_fill_rows(b, tile, side, dx, dy, row);
    if (memcmp(a, b, sizeof(a))) {
        fprintf(stderr, "side %u dx %u dy %u: the fill by rows differs from the word-by-word one\n", side, dx, dy);
        return 1;
    }
    for (i = 0; i < 20000; i++) {           /* the definition, at the corners and at random places */
        x = i < 4 ? (i & 1 ? T - 1 : 0) : rnd() % T;
        y = i < 4 ? (i & 2 ? T - 1 : 0) : rnd() % T;
        if (a8[y * T + x] != t8[((y + dy) % side) * side + (x + dx) % side]) {
            fprintf(stderr, "side %u dx %u dy %u: byte %u,%u is not the tile's\n", side, dx, dy, x, y);
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    static const u32 sides[] = {4, 8, 128, 252, 256, 384, 500, 512, 640, 768, 1024, 1532, 1536};
    u32 k, n, i, side, failed = 0;

    for (k = 0; k < sizeof(sides) / sizeof(sides[0]); k++) {
        side = sides[k];
        for (i = 0; i < side * side / 4; i++)
            tile[i] = rnd();
        failed += one(side, 0, 0);
        failed += one(side, side - 4, side - 1);
        failed += one(side, 0, side - 1);
        failed += one(side, side - 4, 0);
        for (n = 0; n < 4; n++)
            failed += one(side, rnd() % side & ~3u, rnd() % side);
    }
    if (failed)
        return 1;
    printf("ok\n");
    return 0;
}

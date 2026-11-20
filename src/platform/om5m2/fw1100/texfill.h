/* The grain texture's fill (graindrv.c, H2): a square tile of side x side bytes repeated to the firmware's texture of
 * TEX_FILL_TILE x TEX_FILL_TILE bytes with a cyclic shift. Row y of the result is source row (y + dy) mod side, rotated
 * left by dx bytes (a multiple of 4) and repeated across. The source and the result are memory outside the cache (the
 * arena, the texture buffer's uncached alias): every word read or written there costs a bus access, about 0.25 us each
 * on the camera (590 000 reads and as many writes: 278..322 ms a texture with tex_fill_direct).
 *
 * Kept apart from the driver so that the host test (tests/host/test_texfill.c) runs the very code: only plain C here, no
 * firmware address. The includer defines u32 (omk_types.h). */
#ifndef OMK_TEXFILL_H
#define OMK_TEXFILL_H

#define TEX_FILL_TILE 1536
#define TEX_FILL_ROW (TEX_FILL_TILE / 4)    /* words in a row of the result */

/* The plain way: every word of the result read from the source. No work memory: for any CPU */
static inline void tex_fill_direct(volatile u32 *dst, const volatile u32 *src, u32 side, u32 dx, u32 dy)
{
    u32 w = side / 4, y, i, j, r = dy;
    const volatile u32 *s;

    for (y = 0; y < TEX_FILL_TILE; y++) {
        s = src + r * w;
        for (i = 0, j = dx / 4; i < TEX_FILL_ROW; i++) {
            *dst++ = s[j];
            j = j + 1 < w ? j + 1 : 0;
        }
        r = r + 1 < side ? r + 1 : 0;
    }
}

/* The same result with each source row read once: the row is put together - rotated and repeated to the result's width -
 * in `row` (TEX_FILL_ROW words of cached memory the caller owns for the time of the call), then written to every row of
 * the result that shows it: y0 = (r - dy) mod side, then every side rows. side x side / 4 reads of the source instead
 * of TEX_FILL_TILE x TEX_FILL_TILE / 4 (a 512 tile: 65 536 instead of 589 824); the writes are the same words, in
 * another order - the result is written once and never read back, so the order does not matter.
 * side: a multiple of 4, 4..TEX_FILL_TILE; dx a multiple of 4 below side; dy below side (the caller's checks) */
static inline void tex_fill_rows(volatile u32 *dst, const volatile u32 *src, u32 side, u32 dx, u32 dy, u32 *row)
{
    u32 w = side / 4, r, y, i, j;
    const volatile u32 *s;
    volatile u32 *d;

    for (r = 0; r < side; r++) {
        s = src + r * w;
        for (i = 0, j = dx / 4; i < w; i++) {       /* one period, from the source */
            row[i] = s[j];
            j = j + 1 < w ? j + 1 : 0;
        }
        for (; i < TEX_FILL_ROW; i++)               /* the repeats, from the period (cached) */
            row[i] = row[i - w];
        for (y = r >= dy ? r - dy : r + side - dy; y < TEX_FILL_TILE; y += side) {
            d = dst + y * TEX_FILL_ROW;
            for (i = 0; i < TEX_FILL_ROW; i++)
                d[i] = row[i];
        }
    }
}

#endif

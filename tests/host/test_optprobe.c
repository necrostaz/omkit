/* Host test of the optics probe's plain parts (src/platform/om5m2/fw1100/optprobe.h, the code the step runs): the kinds
 * of buffer addresses (window, DRAM, refused), the data inside its allocation, the window check, the sum, and the
 * stripes - bytes 1 and 3 of every group by the stripe's pattern, bytes 0 and 2 as they were, nothing past the pixels
 * asked for. Prints "ok", or what failed. */
#include <stdio.h>
#include <string.h>
#include "omk_types.h"
#include "optprobe.h"

static int failed;

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "%s\n", what);
        failed = 1;
    }
}

/* The data inside its allocation: the start past the allocation's (the small frame's check missed it before) */
static void inside(void)
{
    check(op_inside(0xc0001000u, 0xc0001000u, 0x1000, 0x1000), "the data the whole allocation: inside");
    check(!op_inside(0xc0001000u, 0xc0001000u, 0x1000, 0x1001), "a byte more than the allocation: outside");
    check(op_inside(0xc0001100u, 0xc0001000u, 0x1000, 0xf00), "the data past the start, to the end: inside");
    check(!op_inside(0xc0001100u, 0xc0001000u, 0x1000, 0x1000), "past the start, the length of the allocation: outside");
    check(!op_inside(0xc0000f00u, 0xc0001000u, 0x1000, 16), "the data before the allocation: outside");
    check(!op_inside(0xc0002000u, 0xc0001000u, 0x1000, 0), "the data at the allocation's end: outside");
    check(!op_inside(0xffffff00u, 0x00000100u, 0x1000, 16), "no wrap of the difference: outside");
}

static void kinds(void)
{
    check(op_bus_kind(0xb0000000u, 0x100000) == OP_BUS_WINDOW, "0xb0000000: window");
    check(op_bus_kind(0xc9dd6200u, 40310784) == OP_BUS_WINDOW, "0xc9dd6200 (a texture buffer seen): window");
    check(op_bus_kind(0xdff00000u, 0x100000) == OP_BUS_WINDOW, "the last MiB below 0xe0000000: window");
    check(op_bus_kind(0xdff00000u, 0x100001) == OP_BUS_BAD, "past 0xdfffffff: refused");
    check(op_bus_kind(0xc0000000u, 16) == OP_BUS_BAD, "0xc0000000 (the other processor's): refused");
    check(op_bus_kind(0xc3fffff0u, 16) == OP_BUS_BAD, "0xc3fffff0: refused");
    check(op_bus_kind(0xbffffff0u, 32) == OP_BUS_BAD, "0xbffffff0 + 32 into 0xc0...: refused");
    check(op_bus_kind(0xc4000000u, 16) == OP_BUS_WINDOW, "0xc4000000: window");
    check(op_bus_kind(0xb7fff000u, 0x2000) == OP_BUS_BAD, "across two banks: refused");
    check(op_bus_kind(0xb7fff000u, 0x1000) == OP_BUS_WINDOW, "up to a bank's end: window");
    check(op_bus_kind(0xa5328380u, 0x100000) == OP_BUS_DIRECT, "0xa5328380: DRAM past the cache");
    check(op_bus_kind(0x65328380u, 0x100000) == OP_BUS_DIRECT, "0x65328380: DRAM through the cache");
    check(op_direct(0x65328380u) == 0xa5328380u && op_direct(0xa5328380u) == 0xa5328380u, "the uncached alias");
    check(op_bus_kind(0x60100000u, 16) == OP_BUS_BAD, "the firmware's guarded range (cached): refused");
    check(op_bus_kind(0xa0100000u, 16) == OP_BUS_BAD, "the firmware's guarded range (uncached): refused");
    check(op_bus_kind(0xa0cbe800u, 16) == OP_BUS_DIRECT, "right past the guarded range: DRAM");
    check(op_bus_kind(0xa0cbe7f0u, 32) == OP_BUS_BAD, "into the guarded range's end: refused");
    check(op_bus_kind(0xaffffff0u, 32) == OP_BUS_BAD, "DRAM into 0xb...: refused");
    check(op_bus_kind(0x70000000u, 16) == OP_BUS_BAD, "0x7... (a cached view): refused");
    check(op_bus_kind(0x00001000u, 16) == OP_BUS_BAD, "low memory: refused");
    check(op_bus_kind(0xb0000000u, 0) == OP_BUS_BAD, "an empty range: refused");
}

static void windows(void)
{
    check(op_win_ok(0xb2345678u, 0xb2345678u), "0xb2345678 shown at itself");
    check(op_win_ok(0xc9dd6200u, 0xb1dd6200u), "0xc9dd6200 at 0xb1dd6200");
    check(op_win_ok(0xdadd6200u, 0xbadd6200u), "0xdadd6200 at 0xbadd6200");
    check(!op_win_ok(0xc9dd6200u, 0xb1dd6204u), "another offset: refused");
    check(!op_win_ok(0xc9dd6200u, 0x71dd6200u), "a cached view: refused");
    check(!op_win_ok(0xc9dd6200u, 0xc9dd6200u), "outside the window region: refused");
    check(!op_win_ok(0xc9dd6200u, 0), "none: refused");
}

static void sums(void)
{
    static u32 w[1000];
    u32 i, s = 0;

    for (i = 0; i < 1000; i++) {
        w[i] = i * 0x9e3779b9u;
        s += w[i];
    }
    check(op_read_sum(w, 1000, 0) == s, "the sum");
    check(op_read_sum(w + 500, 500, op_read_sum(w, 500, 0)) == s, "the sum in two parts");
}

/* A picture row of 1100 pixels (550 words) and a guard word after the stripes */
static void stripes(void)
{
    static u32 row[560], before[560];
    const u8 *b = (const u8 *)row, *o = (const u8 *)before;
    u32 i, x, k, px, want1, want3;

    for (i = 0; i < 560; i++)
        row[i] = i * 0x01010101u ^ 0x5a3c96e1u;
    memcpy(before, row, sizeof(row));
    for (px = 0; px <= OP_STRIPE_COLS; px += OP_STRIPE_COLS / 2) {
        memcpy(row, before, sizeof(row));
        op_stripe_row(row, px);
        for (x = 0; x < 560 * 2; x += 2) {
            i = x / 2;
            if (b[4 * i] != o[4 * i] || b[4 * i + 2] != o[4 * i + 2]) {
                fprintf(stderr, "px %u, pixel %u: bytes 0 or 2 changed\n", px, x);
                failed = 1;
                return;
            }
            k = x / OP_STRIPE_PX;
            want1 = x >= px ? o[4 * i + 1] : (k & 3) == 1 ? 255 : (k & 3) == 3 ? 0 : 128;
            want3 = x >= px ? o[4 * i + 3] : (k & 3) == 2 ? 255 : (k & 3) == 3 ? 0 : 128;
            if (b[4 * i + 1] != want1 || b[4 * i + 3] != want3) {
                fprintf(stderr, "px %u, pixel %u: bytes 1, 3 are %u, %u, not %u, %u\n", px, x, b[4 * i + 1], b[4 * i + 3],
                        want1, want3);
                failed = 1;
                return;
            }
        }
    }
}

int main(void)
{
    kinds();
    inside();
    windows();
    sums();
    stripes();
    if (!failed)
        printf("ok\n");
    return failed;
}

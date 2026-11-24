/* The optics probe's plain parts (opticsdrv.c, the step in the look's still jobs): what kind of address a job buffer
 * has, whether the window the firmware gave for it is the one expected, the read with a sum and the stripes written into
 * the picture. Only plain C, no firmware address or call: the host test (tests/host/test_optprobe.c) runs the very code.
 * The includer defines u32 (omk_types.h).
 *
 * Addresses (firmware code): the window map 0x6006a145 takes a bus address 0xb0000000..0xdfffffff except
 * 0xc0000000..0xc3ffffff (the other processor's), and shows its 128 MiB bank in the CPU's window at 0xb0000000 or
 * 0xb8000000 with the same low 27 bits (0xb0... 0xb7... -> 0xb0..., 0xb8... 0xbf... -> 0xb8..., 0xc4... 0xcf... and
 * 0xd0... 0xd7... -> 0xb0..., 0xd8... 0xdf... -> 0xb8...). The window region is past the data cache; its "cached view"
 * is the address - 0x40000000 (0x6006a0b9 converts so). The CPU's own DRAM needs no window: 0x60000000..0x6fffffff
 * through the cache, 0xa0000000..0xafffffff past it. */
#ifndef OMK_OPTPROBE_H
#define OMK_OPTPROBE_H

#define OP_UNCACHED 0x40000000u             /* cached address + this = the same memory past the data cache */
#define OP_GUARD_LO 0x60000200u             /* the firmware range the memory guard watches (cached addresses) */
#define OP_GUARD_HI 0x60cbe800u
#define OP_BANK 0x08000000u                 /* a window shows one 128 MiB bank of the bus */
#define OP_BANK_MASK (OP_BANK - 1)

enum { OP_BUS_BAD, OP_BUS_WINDOW, OP_BUS_DIRECT };

/* The kind of [a, a + len): OP_BUS_WINDOW - a bus address the window map takes, all of it in one bank; OP_BUS_DIRECT -
 * the CPU's DRAM (either alias), past the firmware's guarded range; OP_BUS_BAD - anything else (no access) */
static inline u32 op_bus_kind(u32 a, u32 len)
{
    u32 e = a + len - 1, u;

    if (!len || e < a)
        return OP_BUS_BAD;
    if (a >= 0xb0000000u && e <= 0xdfffffffu) {
        if (e >= 0xc0000000u && a < 0xc4000000u)
            return OP_BUS_BAD;              /* the other processor's region: the map refuses it */
        return (a & OP_BANK_MASK) + len <= OP_BANK ? OP_BUS_WINDOW : OP_BUS_BAD;
    }
    u = a >= 0x60000000u && a < 0x70000000u ? a + OP_UNCACHED : a;
    if (u < 0xa0000000u || u + len - 1 >= 0xb0000000u || u + len - 1 < u)
        return OP_BUS_BAD;
    if (u < OP_GUARD_HI + OP_UNCACHED && u + len > OP_GUARD_LO + OP_UNCACHED)
        return OP_BUS_BAD;                  /* the firmware's own code and data */
    return OP_BUS_DIRECT;
}

/* The uncached address of a DRAM buffer (OP_BUS_DIRECT) */
static inline u32 op_direct(u32 a)
{
    return a >= 0x60000000u && a < 0x70000000u ? a + OP_UNCACHED : a;
}

/* 1 - win is the window of bus as the map makes it: in the window region, the bus address's offset in its bank */
static inline int op_win_ok(u32 bus, u32 win)
{
    return win >= 0xb0000000u && win < 0xc0000000u && (win & OP_BANK_MASK) == (bus & OP_BANK_MASK) &&
           (win & ~OP_BANK_MASK) == (win >= 0xb8000000u ? 0xb8000000u : 0xb0000000u);
}

/* The 32-bit sum of n words added to sum (each word read once) */
static inline u32 op_read_sum(const volatile u32 *p, u32 n, u32 sum)
{
    while (n--)
        sum += *p++;
    return sum;
}

/* The stripes: in the first rows of the picture, OP_STRIPE_PX pixels a stripe, bytes 1 and 3 of every 4-byte group
 * (2 pixels of format 0x302) get a value of the stripe's pattern, bytes 0 and 2 stay. Patterns in turn from the left
 * (byte 1, byte 3): 0 - (128, 128), 1 - (255, 128), 2 - (128, 255), 3 - (0, 0). With the order Y0 Cb Y1 Cr and chroma
 * centred at 128 that is grey, blue, red, green over the picture's own brightness; the other orders and encodings give
 * other pictures (the document of the probe lists them) */
#define OP_STRIPE_PX 64
#define OP_STRIPE_ROWS 256
#define OP_STRIPE_COLS 1024                 /* pixels a row: 16 stripes, the four patterns four times */

/* Arithmetic, not a table: the step runs in a firmware task and reads no table of the core image */
static inline u32 op_stripe_bits(u32 k)
{
    u32 p = k & 3, b1, b3;

    b1 = 128 + 127 * (p == 1) - 128 * (p == 3);
    b3 = 128 + 127 * (p == 2) - 128 * (p == 3);
    return b1 << 8 | b3 << 24;
}

/* One row: px pixels (even) from the row's start, each word read and written once (bytes 0 and 2 kept) */
static inline void op_stripe_row(volatile u32 *row, u32 px)
{
    u32 i, n = px / 2, bits = 0;

    for (i = 0; i < n; i++) {
        if (!(i % (OP_STRIPE_PX / 2)))
            bits = op_stripe_bits(i / (OP_STRIPE_PX / 2));
        row[i] = (row[i] & 0x00ff00ffu) | bits;
    }
}

#endif

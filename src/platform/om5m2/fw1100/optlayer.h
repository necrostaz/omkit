/* The optics layer's plain parts (opticsdrv.c's worker task): Bloom and Halation drawn into the small layer that the ISP
 * then enlarges and adds to the picture. Only plain C, no firmware address or call: the host test
 * (tests/host/test_optlayer.c) runs the very code. The includer defines u8, u16, s32, u32, uptr (omk_types.h).
 *
 * The model. The picture is YCbCr after the tone curve, 8 bits. Light is added in linear light: a code c is
 * lin[c] (Q12, 0..4095) through a power curve between c^2 and c^3 (ol_luts), and back through enc[] (4096 entries). The
 * source of the glow is the excess of a pixel's light over a threshold - one for Bloom, one for Halation - averaged over
 * the layer's cell: from the picture itself where its rows were read (ol_read_row), else from the layer's own Y (the
 * ISP's reduction, which can miss lights smaller than its decimation). Each source is blurred by a Gaussian, or by the
 * sum of two (a narrow and a wide one with a weight, as the glow of a lens model), scaled by its gain, and Halation also
 * by the darkness of the place it falls on. The layer gets, per pixel, the code difference enc[base + added] - enc[base]
 * in Y (base: the layer's own light there - the add is smaller where it is bright already, as light adds up), and in Cb
 * and Cr the halation's share of it times its colour; Bloom is neutral. The ISP then adds Y and the chroma centred at
 * 128 with the gains 1024 (opticsdrv.c's blend hook).
 * The later parts (ol_par_t's em, bdark, hfirst, tmix; each 0 - the first version's values exactly):
 * - the source may be the emission instead of the excess (ol_emission): the tone curve's shoulder undone, a blown pixel
 *   standing for em whites - the excess makes a code of 240 glow half as much as 255, a lamp in the scene is tens of
 *   times brighter than white paper;
 * - Bloom weighted by the darkness of the place, as Halation (on a bright place a filter's glow hardly shows);
 * - Halation's code from the place's own light, Bloom after it: its colour does not fade under a strong Bloom;
 * - Bloom's tail: a wide Gaussian on a plane OL_TAIL times smaller each way, mixed into Bloom's blur (ol_tail_*).
 *
 * The layer's word (format 0x305, 4 bytes a pixel): the byte of Y, Cb and Cr by ol_par_t's positions (the camera's
 * dump decides them, ol_lane_order); the fourth byte 0.
 *
 * The Gaussian (ol_gauss): separable, rows then columns, each a cascade of OL_BOX_PASSES extended boxes - a box of
 * radius r with the next value on either side weighted alpha (Gwosdek, Grewenig, Bruhn, Weickert, "Theoretical
 * foundations of Gaussian convolution by extended box filtering", SSVM 2011): the variance of the cascade is exactly
 * the one asked for (alpha takes the rest), the shape is the Gaussian's by the central limit, and the cost a value is
 * the same whatever the sigma. Sigma here is in layer pixels, Q8; the worker takes off what the ISP's reduction and
 * enlargement smooth anyway (ol_cpu_sigma).
 *
 * The cost. The CPU is a base MN10300 (8 registers; the compiler uses no indexed addressing and puts two nops before
 * every multiplication). Its own mul and mulu are slow (the camera: 10000 turns of a loop with mulu 1.5..2.9 ms against
 * 0.2 ms without, some 25..50 cycles a multiplication); the firmware multiplies with its extension unit instead (udf00,
 * udf01 - thousands of places against a few mul), and so do the loops here (OL_MUL, ol_mulhi). The loops are written
 * for few live values and no test inside them:
 * - a row pass runs over a copy of the row padded with its edge values (or with zeros past the row's non-zero range),
 *   so the loop has no edge test; a column pass goes row by row - the columns' sums in acc[], the input rows it still
 *   needs in a ring - so no loop steps through memory by a row's length (a data cache line is 16 bytes);
 * - each row keeps the range of columns that may be non-zero; a pass works on that range widened by its box, the rest
 *   stays 0 (exactly what the box gives there), so the dark parts of the layer cost next to nothing;
 * - the read of the picture's rows adds one table word a pixel (the excess of Bloom in the low half, of Halation in
 *   the high half); with cells of 8 words (a layer 324 wide on 5184) a cell's words go through one unrolled assembly
 *   loop (ol_read_pack);
 * - the layer's word takes the per-code parts (Halation's weight by darkness, the chroma) from tables, and a pixel
 *   with nothing added is the neutral word at once.
 * The arithmetic is the one of the first version (the host test compares the two): the same rounding and the same
 * order of the passes. No division in the loops (a reciprocal where the means are taken); the tables and the work
 * rows are in the caller's buffer (ol_layout). */
#ifndef OMK_OPTLAYER_H
#define OMK_OPTLAYER_H

#define OL_ONE 4095                         /* linear light Q12: a code of 255 */
#define OL_ENC 4096                         /* entries of enc[] */
#define OL_LANES 4
#define OL_READ_RUN 8                       /* words added in one u32 before its halves are split (16 OL_ONE < 65536) */
#define OL_BOX_PASSES 3
#define OL_SIGMA_MAX (32 * 256)             /* sigma in layer pixels, Q8: the plan's arithmetic stays in 32 bits */
#define OL_R_MAX 31                         /* the largest box radius of a plan (OL_SIGMA_MAX gives 30) */
#define OL_KERNEL __attribute__((noinline))  /* a loop of its own: the compiler keeps its few values in registers */

/* The low word of a x b. On the camera the extension unit's multiply udf01 Dm,Dn (Dn = the low word of Dn x Dm, MULQU of
 * the unit; MDR and the CPU's flags kept, the flags left undefined - hence "cc"): the firmware's own way to multiply.
 * The unit's state (its high word, MCRH, MCRL) is saved and restored by the firmware's interrupt entry (0x60038885 calls
 * 0x60030107, its exit 0x6003011c), so a task switch keeps it; udf01 does not need it anyway */
#ifdef __mn10300__
#define OL_MUL(a, b) ({                                                                                                     \
    u32 ol_m_ = (a);                                                                                                        \
    __asm__("udf01 %1,%0" : "+d"(ol_m_) : "d"((u32)(b)) : "cc");                                                          \
    ol_m_;                                                                                                                  \
})
#else
#define OL_MUL(a, b) ((u32)(a) * (u32)(b))
#endif

/* The high word of x m, both below 2^31 (the unit's signed multiply is then the unsigned one): udf00 Dm,Dn (MULQ: the
 * low word to Dn, the high word to the unit's HI), then udf15 Dn,Dn (GETX: HI to Dn), as the firmware's FixMul16
 * 0x5048367b of the OM-1 does. An interrupt between the two keeps HI (the interrupt entry saves it, see OL_MUL) */
static inline u32 ol_mulhi(u32 x, u32 m)
{
#ifdef __mn10300__
    u32 hi;

    __asm__("udf00 %2,%0\n\tudf15 %1,%1" : "+d"(x), "=d"(hi) : "d"(m) : "cc");
    return hi;
#else
    return (u32)(((unsigned long long)x * m) >> 32);
#endif
}

typedef struct {
    u32 tb, th;                             /* thresholds, linear Q12 */
    u32 sb1, sb2, mixb;                     /* Bloom's Gaussians: sigmas in layer pixels Q8 (0 - none), the second's
                                               weight Q8 (0 - one Gaussian) */
    u32 sh1, sh2, mixh;                     /* the same for Halation */
    u32 gb, gh;                             /* gains of the added light, Q8 (256 - the source once) */
    u32 dark;                               /* Halation weighted by darkness: Q8, 0 - not at all, 256 - none on white */
    s32 hcb, hcr;                           /* Halation's chroma a unit of its Y code, Q8 signed */
    u32 py, pcb, pcr;                       /* the bytes (0..3) of Y, Cb and Cr in a layer word */
    /* The later parts; 0 each - the first version's values */
    u32 em;                                 /* the source: 0 - the excess over the threshold; 1..OL_EM_MAX - the emission
                                               of a pixel whose blown light stands for em whites (ol_emission) */
    u32 bdark;                              /* Bloom weighted by darkness as Halation: Q8 */
    u32 hfirst;                             /* 1 - Halation's code (and so its colour) from the place's own light, Bloom
                                               added after it; 0 - Bloom first */
    u32 st, tmix;                           /* Bloom's tail: a Gaussian of st (pixels of the tail's plane, Q8) on the
                                               sources reduced OL_TAIL times (ol_tail_*), mixed in with the weight tmix
                                               (Q8; 0 - no tail) */
} ol_par_t;

#define OL_EM_MAX 64                        /* whites a blown pixel stands for, at most: em OL_ONE stays below 2^18 */
#define OL_TAIL 4                           /* the tail's plane: cells of OL_TAIL x OL_TAIL layer pixels */

enum { OL_ROWS, OL_COLS };                  /* the passes, for the pace callback */

typedef struct {
    u32 lw, lh;                             /* the layer */
    u8 *y;                                  /* lw * lh: the layer's Y as read */
    u16 *pb, *ph;                           /* lw * lh: the sources of Bloom and Halation, then their blurs */
    u16 *pc;                                /* lw * lh: the second Gaussian's copy */
    u16 *tmp;                               /* lw + 2 OL_R_MAX + 6: a row padded with its edges */
    u32 *acc;                               /* lw: a column pass's sums */
    u16 *ring;                              /* (OL_R_MAX + 2) rows of lw: the input rows a column pass still needs */
    u16 *rng;                               /* 4 lh: each row's columns [x0, x1) that may be non-zero; a column pass's
                                               new ones after them */
    u8 *nrow;                               /* lh: rows of the picture read into each row of cells */
    u8 *cwp;                                /* lw: pixel pairs of the picture in each column of cells */
    u16 *ry;                                /* lh: the picture's first row of each row of cells (ol_rows) */
    const u16 *lin;                         /* 256 */
    const u8 *enc;                          /* OL_ENC */
    u32 *tab;                               /* 256: the read's excess of Bloom | of Halation << 16 */
    u32 *cw;                                /* 256: a word's chroma bytes by the halation's Y code */
    u16 *wd;                                /* 256: Halation's weight by the darkness of the place, Q8, by its code */
    u16 *wb;                                /* 256: the same of Bloom */
    u16 *pt;                                /* tw * th: Bloom's tail (ol_tail_*) */
    u32 tw, th;                             /* the tail's plane: the layer's size / OL_TAIL, rounded up */
    u32 shift;                             /* the read's sums are kept >> shift (u16) */
    u32 c8;                                 /* 1 - every cell is OL_READ_RUN words wide: the read packs (ol_read_pack) */
    u32 rbytes;                             /* 1 - the packing read loads bytes (the picture through the cache), 0 - words
                                               (past it: each word read once) */
    u32 neutral, ys;                        /* the neutral word; the shift of Y in a word */
    u32 (*pace)(void *ctx, u32 part);       /* after each pass of a Gaussian (OL_ROWS, OL_COLS): not 0 - stop; 0 - none */
    void *ctx;
    u32 done, all;                          /* values the passes worked on; of the whole planes */
} ol_t;

/* The work buffer's parts for a layer lw x lh from address b (4-aligned): the pointers set in o (lin and enc too: the
 * caller copies the curves there); the end address. With b 0 and a scratch o: the bytes needed */
static inline uptr ol_layout(ol_t *o, uptr b, u32 lw, u32 lh)
{
    u32 n = (lw * lh + 1) & ~1u;            /* the planes stay 4-aligned */

    o->lw = lw;
    o->lh = lh;
    o->tw = (lw + OL_TAIL - 1) / OL_TAIL;
    o->th = (lh + OL_TAIL - 1) / OL_TAIL;
    o->lin = (const u16 *)b;
    b += 512;
    o->enc = (const u8 *)b;
    b += OL_ENC;
    o->tab = (u32 *)b;
    b += 1024;
    o->cw = (u32 *)b;
    b += 1024;
    o->wd = (u16 *)b;
    b += 512;
    o->wb = (u16 *)b;
    b += 512;
    o->pt = (u16 *)b;
    b += (2 * o->tw * o->th + 3) & ~3u;
    o->acc = (u32 *)b;
    b += 4 * lw;
    o->pb = (u16 *)b;
    b += 2 * n;
    o->ph = (u16 *)b;
    b += 2 * n;
    o->pc = (u16 *)b;
    b += 2 * n;
    o->ring = (u16 *)b;
    b += (2 * (OL_R_MAX + 2) * lw + 3) & ~3u;
    o->tmp = (u16 *)b;
    b += (2 * (lw + 2 * OL_R_MAX + 6) + 3) & ~3u;
    o->rng = (u16 *)b;
    b += 8 * lh;
    o->ry = (u16 *)b;
    b += (2 * lh + 3) & ~3u;
    o->y = (u8 *)b;
    b += n;
    o->nrow = (u8 *)b;
    b += lh;
    o->cwp = (u8 *)b;
    b += lw;
    return b;
}

/* Core: the curves. gamma 200..300 (x100): lin = ((300 - g) x^2 + (g - 200) x^3) / 100 of OL_ONE; enc its inverse
 * (the nearest code at or below). Out-of-range gammas are clamped */
static inline void ol_luts(u32 gamma, u16 *lin, u8 *enc)
{
    u32 c, v, l2, l3, k;

    if (gamma < 200)
        gamma = 200;
    if (gamma > 300)
        gamma = 300;
    for (c = 0; c < 256; c++) {
        l2 = c * c * OL_ONE / 65025;
        l3 = (c * c * c / 255) * OL_ONE / 65025;
        lin[c] = (u16)(((300 - gamma) * l2 + (gamma - 200) * l3 + 50) / 100);
    }
    lin[255] = OL_ONE;
    for (v = 0, k = 0; v < OL_ENC; v++) {
        while (k < 255 && lin[k + 1] <= v)
            k++;
        enc[v] = (u8)k;
    }
}

/* n u16 values from s to d: by words where both allow it */
static OL_KERNEL void ol_copy16(u16 *d, const u16 *s, u32 n)
{
    u32 *dw, *de;
    const u32 *sw;

    if (((uptr)d ^ (uptr)s) & 2) {
        while (n--)
            *d++ = *s++;
        return;
    }
    if (n && ((uptr)d & 2)) {
        *d++ = *s++;
        n--;
    }
    dw = (u32 *)d;
    sw = (const u32 *)s;
    for (de = dw + n / 2; dw != de;)
        *dw++ = *sw++;
    if (n & 1)
        *(u16 *)dw = *(const u16 *)sw;
}

/* n u16 values at d set to 0: by words where it can */
static OL_KERNEL void ol_zero16(u16 *d, u32 n)
{
    u32 *dw, *de;

    if (n && ((uptr)d & 2)) {
        *d++ = 0;
        n--;
    }
    dw = (u32 *)d;
    for (de = dw + n / 2; dw != de;)
        *dw++ = 0;
    if (n & 1)
        *(u16 *)dw = 0;
}

/* The pairs of picture pixels in each column of cells (width W, even, layer lw): cwp[cx]; 1 - a column has none or more
 * than 255 (the layer too small for the picture: not used) */
static inline int ol_columns(u8 *cwp, u32 W, u32 lw)
{
    u32 p, cx = 0, acc = 0, n = 0, edge = W;

    for (p = 0; p < W / 2; p++) {
        n++;
        acc += 2 * lw;                      /* the pair's right edge in cells x W */
        if (acc >= edge) {                  /* edge: (cx + 1) W */
            if (n > 255)
                return 1;
            cwp[cx++] = (u8)n;
            edge += W;
            n = 0;
        }
    }
    if (n && cx < lw) {
        if (n > 255)
            return 1;
        cwp[cx++] = (u8)n;
    }
    for (p = 0; p < lw; p++)
        if (p >= cx || !cwp[p])
            return 1;
    return 0;
}

/* The shift that keeps the read's sums in u16: a cell column of at most cwmax pairs, read in at most passes rows */
static inline u32 ol_read_shift(const u8 *cwp, u32 lw, u32 passes)
{
    u32 i, m = 0, s = 0;

    for (i = 0; i < lw; i++)
        if (cwp[i] > m)
            m = cwp[i];
    while (((2 * m * passes * OL_ONE) >> s) > 65535)
        s++;
    return s;
}

/* The shoulder's inverse: a picture's tone curve rolls a scene's light L off into x = L / (1 + L (1 - 1/m)) (0..1 for
 * L 0..m), so L = x / (1 - x (1 - 1/m)); a blown pixel (x = 1) stands for m whites. Here L / m in Q12 from x in Q12:
 * x OL_ONE / (m OL_ONE - (m - 1) x) - OL_ONE at x = OL_ONE, x / m near 0 (x OL_ONE below 2^24, m OL_ONE below 2^18) */
static inline u32 ol_shoulder(u32 x, u32 m)
{
    return x * OL_ONE / (m * OL_ONE - (m - 1) * x);
}

/* The emission of a pixel of linear light x (Q12) over the threshold t (Q12, below OL_ONE): the shoulder's light above
 * the threshold's, OL_ONE at a blown pixel - so a code of 245 emits ~0.35 of a blown one at m 16 and a threshold of 200,
 * 230 ~0.13, where the excess x - t gives 0.79 and 0.41 (the light of a lamp and of white paper, both near 255 in the
 * picture, are far apart in the scene) */
static inline u32 ol_emission(u32 x, u32 t, u32 m)
{
    u32 s = ol_shoulder(x, m), q = ol_shoulder(t, m);

    return s > q ? (s - q) * OL_ONE / (OL_ONE - q) : 0;
}

/* The read's table (after ol_columns): tab[c] = the source of code c for Bloom | for Halation << 16 - the excess over
 * the threshold, or the emission (p->em). A half holds the sum of 16 pixels (16 OL_ONE < 65536), so the read adds at
 * most 8 words before it splits the halves */
static inline void ol_read_table(ol_t *o, const ol_par_t *p)
{
    u32 i, l, eb, eh, m = p->em > OL_EM_MAX ? OL_EM_MAX : p->em;

    for (i = 0; i < 256; i++) {
        l = o->lin[i];
        if (m) {
            eb = ol_emission(l, p->tb, m);
            eh = ol_emission(l, p->th, m);
        } else {
            eb = l > p->tb ? l - p->tb : 0;
            eh = l > p->th ? l - p->th : 0;
        }
        o->tab[i] = eb | eh << 16;
    }
}

/* After ol_columns: whether every cell is OL_READ_RUN words wide (o->c8: the packing read), and the picture's first row
 * of each row of cells, ry[cy] = cy H / lh (H the picture's height, below 65536) */
static inline void ol_rows(ol_t *o, u32 H)
{
    u32 i;

    for (i = 0, o->c8 = 1; i < o->lw; i++)
        if (o->cwp[i] != OL_READ_RUN)
            o->c8 = 0;
    for (i = 0; i < o->lh; i++)
        o->ry[i] = (u16)(i * H / o->lh);
}

/* The bit reversal of 4 bits: the order of a read's passes, each one halving the gaps the ones before left */
static inline u32 ol_bitrev4(u32 j)
{
    return (j & 1) << 3 | (j & 2) << 1 | (j & 4) >> 1 | (j & 8) >> 3;
}

/* A word of a table at a byte offset: the CPU's indexed load (mov (Di,Am),Dn of the base instruction set, which the
 * firmware's own code uses throughout; the compiler does not make it for this CPU), plain C elsewhere */
#ifdef __mn10300__
#define OL_TABW(t, off) ({                                                                                                  u32 ol_v_;                                                                                                              __asm__("mov (%1,%2),%0" : "=d"(ol_v_) : "d"(off), "a"(t));                                                             ol_v_;                                                                                                              })
#else
#define OL_TABW(t, off) (*(const u32 *)((const u8 *)(t) + (off)))
#endif

/* One row of the picture (4:2:2, words Y0 Cb Y1 Cr) into the sums of a row of cells db[], dh[] (lw each): per cell
 * column, the excess over each threshold of its pixels, >> shift - the same sums as the excess added pixel by pixel.
 * Each word is read once (past the cache a read is the cost); the table is not written while this runs (it is made
 * before, in another function) */
static OL_KERNEL u32 ol_read_run(const volatile u32 *row, const volatile u32 *end, const u32 *tab)
{
    u32 s = 0, w;

    do {
        w = *row++;
        s += OL_TABW(tab, (w & 0xff) << 2) + OL_TABW(tab, w >> 14 & 0x3fc);
    } while (row != end);
    return s;
}

/* A row of cells of OL_READ_RUN words each (o->c8) into packed sums: out[cx] = the cell's excess of Bloom | of
 * Halation << 16 (16 pixels: each half below 65536), for out .. end (at least one cell). bytes 1: the two Y bytes of a
 * word loaded as bytes (through the cache a load costs the same, and no shift or mask is needed); 0: each word loaded
 * once and split (past the cache each load goes to the bus). On the camera an unrolled loop of 8 (bytes) or 10 (words)
 * instructions a word; the same sums as ol_read_run (drv_optics_layer checks the two against each other at start) */
static OL_KERNEL void ol_read_pack(const volatile u32 *row, u32 *out, const u32 *end, const u32 *tab, u32 bytes)
{
#ifdef __mn10300__
    u32 s, a, b;

    if (bytes)
        __asm__ __volatile__("1:\n\tclr %0\n\t"
                             ".irp k,0,4,8,12,16,20,24,28\n\t"
                             "movbu (\\k,%3),%1\n\tmovbu (\\k+2,%3),%2\n\t"
                             "asl2 %1\n\tasl2 %2\n\t"
                             "mov (%1,%5),%1\n\tmov (%2,%5),%2\n\t"
                             "add %1,%0\n\tadd %2,%0\n\t"
                             ".endr\n\t"
                             "add 32,%3\n\tmov %0,(%4)\n\tinc4 %4\n\tcmp %6,%4\n\tbne 1b"
                             : "=&d"(s), "=&d"(a), "=&d"(b), "+a"(row), "+a"(out)
                             : "a"(tab), "a"(end)
                             : "cc", "memory");
    else
        __asm__ __volatile__("1:\n\tclr %0\n\t"
                             ".irp k,0,4,8,12,16,20,24,28\n\t"
                             "mov (\\k,%3),%1\n\tmov %1,%2\n\t"
                             "asl2 %1\n\tlsr 14,%2\n\t"
                             "and 1020,%1\n\tand 1020,%2\n\t"
                             "mov (%1,%5),%1\n\tmov (%2,%5),%2\n\t"
                             "add %1,%0\n\tadd %2,%0\n\t"
                             ".endr\n\t"
                             "add 32,%3\n\tmov %0,(%4)\n\tinc4 %4\n\tcmp %6,%4\n\tbne 1b"
                             : "=&d"(s), "=&d"(a), "=&d"(b), "+a"(row), "+a"(out)
                             : "a"(tab), "a"(end)
                             : "cc", "memory");
#else
    (void)bytes;
    do {
        *out++ = ol_read_run(row, row + OL_READ_RUN, tab);
        row += OL_READ_RUN;
    } while (out != end);
#endif
}

/* The packed sums of ol_read_pack added into db[], dh[] (each half >> shift, as ol_read_row adds a run's halves) */
static OL_KERNEL void ol_read_split(const u32 *s, u16 *db, u16 *dh, const u16 *de, u32 shift)
{
    do {
        u32 v = *s++;

        *db = (u16)(*db + ((v & 0xffff) >> shift));
        *dh = (u16)(*dh + (v >> 16 >> shift));
        db++;
        dh++;
    } while (db != de);
}

static OL_KERNEL void ol_read_row(const ol_t *o, const volatile u32 *row, u16 *db, u16 *dh)
{
    const u8 *c = o->cwp, *ce = c + o->lw;
    u32 n, k, s, b, h;

    if (o->c8) {                            /* o->acc: not in use while the picture is read */
        ol_read_pack(row, o->acc, o->acc + o->lw, o->tab, o->rbytes);
        ol_read_split(o->acc, db, dh, db + o->lw, o->shift);
        return;
    }
    do {
        n = *c++;
        b = h = 0;
        do {
            k = n > OL_READ_RUN ? OL_READ_RUN : n;
            n -= k;
            s = ol_read_run(row, row + k, o->tab);
            row += k;
            b += s & 0xffff;
            h += s >> 16;
        } while (n);
        *db = (u16)(*db + (b >> o->shift));
        *dh = (u16)(*dh + (h >> o->shift));
        db++;
        dh++;
    } while (c != ce);
}

/* The row into the sources' sums of the row of cells cy */
static inline void ol_read_cells(ol_t *o, const volatile u32 *row, u32 cy)
{
    ol_read_row(o, row, o->pb + cy * o->lw, o->ph + cy * o->lw);
    if (o->nrow[cy] < 255)
        o->nrow[cy]++;
}

/* The means of a row of cells read k times: floor((x << shift) / d), d = 2 cwp k, as the high word of (x << shift) m with
 * m = 0xffffffff / d + 1 - exact while (x << shift) d < 2^32 - and both factors below 2^31 for ol_mulhi (d >= 4: see
 * ol_sources); the reciprocal made again only where the column's pairs change (a layer 324 wide: never) */
static OL_KERNEL void ol_means_row(u16 *pb, u16 *ph, const u8 *cwp, const u8 *ce, u32 k, u32 shift)
{
    u32 c = 0, m = 0;

    do {
        if (*cwp != c) {
            c = *cwp;
            m = 0xffffffffu / (2 * c * k) + 1;
        }
        cwp++;
        *pb = (u16)ol_mulhi((u32)*pb << shift, m);
        *ph = (u16)ol_mulhi((u32)*ph << shift, m);
        pb++;
        ph++;
    } while (cwp != ce);
}

/* After the read (o->pb, o->ph hold sums where nrow > 0, anything elsewhere): the mean excess of a pixel, Q12, in every
 * cell - from the sums where the picture's rows were read, from the layer's Y elsewhere (through the read's table: the
 * same excess as of a pixel). The division by a reciprocal is exact while (65535 << shift) 2 cwp nrow < 2^32
 * (ol_means_row); a narrower layer (cells of more pairs) divides */
static OL_KERNEL void ol_sources(ol_t *o, const ol_par_t *p)
{
    u32 cx, cy, i, t, d, k, cm = 0, cn = 255, recip, lw = o->lw;
    u16 *pb = o->pb, *ph = o->ph;
    const u8 *y;

    ol_read_table(o, p);
    for (cx = 0; cx < lw; cx++) {
        if (o->cwp[cx] > cm)
            cm = o->cwp[cx];
        if (o->cwp[cx] < cn)
            cn = o->cwp[cx];
    }
    recip = o->shift < 16 && cn >= 2 && (65535u << o->shift) < 0xffffffffu / (2 * cm * 255);
    for (cy = 0, i = 0; cy < o->lh; cy++, i += lw) {
        k = o->nrow[cy];
        if (k && recip) {
            ol_means_row(pb + i, ph + i, o->cwp, o->cwp + lw, k, o->shift);
        } else if (k) {
            for (cx = 0; cx < lw; cx++) {
                d = 2 * (u32)o->cwp[cx] * k;
                pb[i + cx] = (u16)(((u32)pb[i + cx] << o->shift) / d);
                ph[i + cx] = (u16)(((u32)ph[i + cx] << o->shift) / d);
            }
        } else {
            for (cx = 0, y = o->y + i; cx < lw; cx++) {
                t = o->tab[y[cx]];
                pb[i + cx] = (u16)t;
                ph[i + cx] = (u16)(t >> 16);
            }
        }
    }
}

/* The integer square root */
static inline u32 ol_isqrt(u32 v)
{
    u32 r = 0, b = 1u << 30;

    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

/* The extended box for one pass of the cascade: radius *r and the end weight *alpha (Q8) whose variance is sigma^2 /
 * OL_BOX_PASSES (sigma Q8, at most OL_SIGMA_MAX). The variance of a box of radius r is r (r + 1) / 3; with the ends at
 * weight a: (r (r + 1) (2r + 1) / 3 + 2a (r + 1)^2) / (2r + 1 + 2a) - solved for a */
static inline void ol_box_plan(u32 sigma, u32 *r, u32 *alpha)
{
    u32 v = sigma * sigma / OL_BOX_PASSES >> 8, k = 0, num, den;   /* v: the variance a pass, Q8 */

    while ((k + 1) * (k + 2) * 256 / 3 <= v)
        k++;
    num = v * (2 * k + 1) - k * (k + 1) * (2 * k + 1) * 256 / 3;
    den = 2 * (k + 1) * (k + 1) * 256 - 2 * v;
    *r = k;
    *alpha = den ? (num << 8) / den : 0;
    if (*alpha > 255)
        *alpha = 255;
}

/* The box's weights: the sum of the box S and of its two ends E give (S + a E / 256) / (2r + 1 + 2a / 256) as
 * (S ia + E ib + 32768) >> 16 (values up to OL_ONE, r below 64: in 32 bits) */
static inline void ol_box_weights(u32 r, u32 alpha, u32 *ia, u32 *ib)
{
    u32 w = (2 * r + 1) * 256 + 2 * alpha;

    *ia = (65536u * 256 + w / 2) / w;
    *ib = alpha * *ia >> 8;
}

/* One extended box along a row of n values at v, the values [s, e) of it made (e > s; outside the range the row is 0):
 * through tmp - the row's [s, e) with r + 1 values on either side: the row's edge value at its ends, 0 inside it (past
 * a range the row is 0) - so the loop has no edge test */
static OL_KERNEL void ol_box_row(u16 *v, u32 n, u32 s, u32 e, u32 r, u32 ia, u32 ib, u16 *tmp)
{
    u32 i, sum, lp = s ? 0 : v[0], rp = e < n ? 0 : v[n - 1];
    u16 *P = tmp + ((((uptr)(v + s) ^ (uptr)(tmp + r + 1)) & 2) ? 1 : 0), *out = v + s, *end = v + e;
    const u16 *lo, *hi;

    for (i = 0; i <= r; i++) {
        P[i] = (u16)lp;
        P[r + 1 + e - s + i] = (u16)rp;
    }
    ol_copy16(P + r + 1, v + s, e - s);
    for (sum = 0, i = 1; i <= 2 * r + 1; i++)
        sum += P[i];
    lo = P;                                 /* P[i]: the value r + 1 before; P[i + 2r + 2]: r + 1 after */
    hi = P + 2 * r + 2;
    do {
        u32 l = *lo++, h = *hi++;

        *out++ = (u16)((OL_MUL(sum, ia) + OL_MUL(l + h, ib) + 32768) >> 16);
        sum += h - *lo;
    } while (out != end);
}

/* The non-zero range [x0, x1) of each row of a plane (x0 >= x1: the row is 0); a row looked at by words where it can
 * be (most rows of a dark layer are 0) */
static OL_KERNEL void ol_ranges(ol_t *o, const u16 *plane)
{
    u32 y, a, b, lw = o->lw, n2 = lw / 2;
    const u16 *row;
    const u32 *w;

    for (y = 0, row = plane; y < o->lh; y++, row += lw) {
        a = 0;
        if (!(lw & 1) && !((uptr)row & 3)) {
            for (w = (const u32 *)row; a < n2 && !w[a]; a++)
                ;
            a *= 2;
        }
        for (; a < lw && !row[a]; a++)
            ;
        for (b = lw; b > a && !row[b - 1]; b--)
            ;
        o->rng[2 * y] = (u16)a;
        o->rng[2 * y + 1] = (u16)(a < b ? b : 0);
    }
}

/* The rows' pass of the box (r, ia, ib) over a plane: each row on its range widened by r + 1 */
static inline void ol_box_rows(ol_t *o, u16 *plane, u32 r, u32 ia, u32 ib)
{
    u32 y, s, e, lw = o->lw;
    u16 *rg = o->rng, *row;

    for (y = 0, row = plane; y < o->lh; y++, rg += 2, row += lw) {
        if (rg[0] >= rg[1])
            continue;                       /* a row of 0 stays 0 */
        s = rg[0] > r + 1 ? rg[0] - r - 1 : 0;
        e = rg[1] + r + 1 < lw ? rg[1] + r + 1 : lw;
        ol_box_row(row, lw, s, e, r, ia, ib, o->tmp);
        rg[0] = (u16)s;
        rg[1] = (u16)e;
        o->done += e - s;
    }
}

/* A column pass's row [s, e): out = (acc ia + (lo + hi) ib + 32768) >> 16, then acc += hi - lo1 (each a value) */
static OL_KERNEL void ol_cols_row(u16 *out, u16 *end, u32 *ac, const u16 *lo, const u16 *lo1, const u16 *hi, u32 ia,
                                  u32 ib)
{
    do {
        u32 v = *ac, h = *hi++;

        *out++ = (u16)((OL_MUL(v, ia) + OL_MUL(*lo++ + h, ib) + 32768) >> 16);
        *ac++ = v + h - *lo1++;
    } while (out != end);
}

/* The same two columns a step (ol_box_cols with pairs): the rows and acc read and written by words - a word holds the
 * values of columns 2i (low half) and 2i + 1. First E = lo + hi into the output row itself (its input is in the ring
 * already; two sums below 2 OL_ONE + 2: no carry between the halves) ... */
static OL_KERNEL void ol_ends_pairs(u32 *out, u32 *end, const u32 *lo, const u32 *hi)
{
    do {
        *out++ = *lo++ + *hi++;
    } while (out != end);
}

/* ... then the output over it: (acc ia + E ib + 32768) >> 16 for both halves ... */
static OL_KERNEL void ol_out_pairs(u32 *out, u32 *end, const u32 *ac, u32 ia, u32 ib)
{
    do {
        u32 a = *ac++, e = *out, v;

        v = (OL_MUL(a & 0xffff, ia) + OL_MUL(e & 0xffff, ib) + 32768) >> 16;
        *out++ = v | (OL_MUL(a >> 16, ia) + OL_MUL(e >> 16, ib) + 32768) >> 16 << 16;
    } while (out != end);
}

/* ... then the sums: acc += hi - lo1 in both halves at once (each half's result is in 0..65535, so the word's is the
 * halves' exactly) */
static OL_KERNEL void ol_acc_pairs(u32 *ac, u32 *end, const u32 *lo1, const u32 *hi)
{
    do {
        *ac = *ac + *hi++ - *lo1++;
        ac++;
    } while (ac != end);
}

/* The columns' pass of the box (r, ia, ib) over a plane, row by row: acc[x] the sum of column x over the rows y - r ..
 * y + r (edges clamped), the ring the input rows y - r - 1 .. y (the plane's rows above y hold the output already), the
 * rows below y the plane's own. Row y is made over the union of the ranges of the input rows y - r - 1 .. y + r + 1
 * (outside it every value the row needs is 0, so is acc and so is the output). With a box of r <= OL_PAIRS_R and an even
 * width the columns go two a step: a sum of 2r + 1 values (each at most OL_ONE + 1, the rounding) stays in 16 bits */
#define OL_PAIRS_R 7
static inline void ol_box_cols(ol_t *o, u16 *plane, u32 r, u32 ia, u32 ib)
{
    u32 lw = o->lw, lh = o->lh, nr = r + 2, pairs = r <= OL_PAIRS_R && !(lw & 1), y, j, jl, jh, s, e, a, b, rl, k;
    u16 *rin = o->rng, *rout = o->rng + 2 * lh, *cur, *slot, *n1, *n2, *ring = o->ring, *rend, *last;
    u16 rr[2 * (OL_R_MAX + 2)];             /* each ring row's range that may be non-zero (all of it at first) */
    const u16 *lo, *lo1, *hi;
    u32 *acc = o->acc;

    for (y = 0; y < lh; y++) {              /* the output ranges, from the input ones */
        jl = y > r ? y - r - 1 : 0;
        jh = y + r + 1 < lh ? y + r + 1 : lh - 1;
        for (a = lw, b = 0, j = jl; j <= jh; j++)
            if (rin[2 * j] < rin[2 * j + 1]) {
                if (rin[2 * j] < a)
                    a = rin[2 * j];
                if (rin[2 * j + 1] > b)
                    b = rin[2 * j + 1];
            }
        if (pairs && a < b) {
            a &= ~1u;                       /* a value made past the range is 0, as it is */
            b = (b + 1) & ~1u;
        }
        rout[2 * y] = (u16)a;
        rout[2 * y + 1] = (u16)b;
    }
    for (j = 0; j < lw; j++)
        acc[j] = 0;
    for (j = 0; j <= 2 * r; j++) {          /* acc: the rows -r .. r, the ones above the first clamped to it */
        y = j <= r ? 0 : j - r < lh ? j - r : lh - 1;
        cur = plane + OL_MUL(y, lw);
        s = rin[2 * y];
        e = rin[2 * y + 1];
        if (pairs) {
            for (s >>= 1, e = (e + 1) >> 1; s < e; s++)
                acc[s] += ((const u32 *)cur)[s];
        } else {
            for (; s < e; s++)
                acc[s] += cur[s];
        }
    }
    rend = ring + OL_MUL(nr, lw);           /* the ring's row of y: y % nr, by a pointer that wraps */
    rl = OL_MUL(r + 1, lw);
    last = plane + OL_MUL(lh - 1, lw);
    for (k = 0; k < nr; k++) {
        rr[2 * k] = 0;
        rr[2 * k + 1] = (u16)lw;
    }
    for (y = 0, k = 0, cur = plane, slot = ring; y < lh; y++, cur += lw, slot = n1, k = k + 1 == nr ? 0 : k + 1) {
        if (rr[2 * k] < rr[2 * k + 1])      /* the row in the ring: its range copied, the rest 0 (as it is) */
            ol_zero16(slot + rr[2 * k], rr[2 * k + 1] - rr[2 * k]);
        a = rin[2 * y];
        b = rin[2 * y + 1];
        if (a < b)
            ol_copy16(slot + a, cur + a, b - a);
        rr[2 * k] = (u16)a;
        rr[2 * k + 1] = (u16)b;
        n1 = slot + lw == rend ? ring : slot + lw;  /* (y + 1) % nr - for y > r also (y - r - 1) % nr */
        s = rout[2 * y];
        e = rout[2 * y + 1];
        if (s >= e)
            continue;                       /* every input row of it 0: the row stays 0, acc too */
        n2 = n1 + lw == rend ? ring : n1 + lw;      /* (y + 2) % nr: for y > r (y - r) % nr */
        hi = (y + r + 1 < lh ? cur + rl : y + 1 < lh ? last : slot) + s;  /* row min(y + r + 1, lh - 1) */
        lo = (y > r ? n1 : ring) + s;
        lo1 = (y > r ? n2 : ring) + s;
        if (pairs) {
            ol_ends_pairs((u32 *)(cur + s), (u32 *)(cur + e), (const u32 *)lo, (const u32 *)hi);
            ol_out_pairs((u32 *)(cur + s), (u32 *)(cur + e), acc + s / 2, ia, ib);
            ol_acc_pairs(acc + s / 2, acc + e / 2, (const u32 *)lo1, (const u32 *)hi);
        } else {
            ol_cols_row(cur + s, cur + e, acc + s, lo, lo1, hi, ia, ib);
        }
        o->done += e - s;
    }
    for (y = 0; y < 2 * lh; y++)
        rin[y] = rout[y];
}

/* The Gaussian of sigma (layer pixels, Q8) over a plane, rows then columns; sigma below 0.25 pixel: nothing. 0, or 1 -
 * stopped by the pace callback (the plane half done) */
static inline u32 ol_gauss(ol_t *o, u16 *plane, u32 sigma)
{
    u32 r, a, ia, ib, k;

    if (sigma < 64)
        return 0;
    if (sigma > OL_SIGMA_MAX)
        sigma = OL_SIGMA_MAX;
    ol_box_plan(sigma, &r, &a);
    ol_box_weights(r, a, &ia, &ib);
    ol_ranges(o, plane);
    o->all += 2 * OL_BOX_PASSES * o->lw * o->lh;
    for (k = 0; k < OL_BOX_PASSES; k++) {
        ol_box_rows(o, plane, r, ia, ib);
        if (o->pace && o->pace(o->ctx, OL_ROWS))
            return 1;
        ol_box_cols(o, plane, r, ia, ib);
        if (o->pace && o->pace(o->ctx, OL_COLS))
            return 1;
    }
    return 0;
}

/* A source blurred by one Gaussian (s2 or mix 0) or by (256 - mix) G(s1) + mix G(s2), the copy through o->pc. 0, or 1 -
 * stopped */
static inline u32 ol_blur2(ol_t *o, u16 *plane, u32 s1, u32 s2, u32 mix)
{
    u32 i, n = o->lw * o->lh;

    if (!mix || !s2)
        return ol_gauss(o, plane, s1);
    if (mix > 256)
        mix = 256;
    for (i = 0; i < n; i++)
        o->pc[i] = plane[i];
    if (ol_gauss(o, plane, s1) || ol_gauss(o, o->pc, s2))
        return 1;
    for (i = 0; i < n; i++)
        plane[i] = (u16)((OL_MUL(plane[i], 256 - mix) + OL_MUL(o->pc[i], mix) + 128) >> 8);
    return 0;
}

/* Bloom's tail (a diffusion filter's glow reaches 150..400 px of the picture, ~8..12 % of its width - far wider than
 * its near part, and too wide for the layer's own plane in the time: ~19 layer pixels of sigma make a box of 11 a pass,
 * no paired columns, and the whole plane lit). It is made on a plane OL_TAIL times smaller each way, as one level of a
 * pyramid: the Bloom's sources reduced by a mean over each cell (ol_tail_down, before the Bloom's own Gaussian), the
 * Gaussian there (ol_tail_blur), then enlarged back bilinearly and mixed into the Bloom's blur with its weight
 * (ol_tail_up): pb = (256 - w) G(pb) + w T. The mean and the bilinear enlargement keep the light's sum. */

/* The tail's sources: pt[cell] = the mean of the Bloom's sources over the cell (o->acc: the sums of a row of cells) */
static OL_KERNEL void ol_tail_down(ol_t *o)
{
    u32 lw = o->lw, tw = o->tw, ty, tx, y, y0, y1, x, n, k, s;
    u32 *acc = o->acc;
    const u16 *row;
    u16 *out = o->pt;

    for (ty = 0; ty < o->th; ty++) {
        y0 = ty * OL_TAIL;
        y1 = y0 + OL_TAIL < o->lh ? y0 + OL_TAIL : o->lh;
        for (tx = 0; tx < tw; tx++)
            acc[tx] = 0;
        for (y = y0, row = o->pb + OL_MUL(y0, lw); y < y1; y++, row += lw) {
            for (tx = 0, x = 0; x + OL_TAIL <= lw; tx++, x += OL_TAIL)
                acc[tx] += (u32)row[x] + row[x + 1] + row[x + 2] + row[x + 3];
            for (; x < lw; x++)             /* a last cell narrower than OL_TAIL */
                acc[tx] += row[x];
        }
        for (tx = 0, x = 0; tx < tw; tx++, x += OL_TAIL) {
            s = acc[tx];
            if (y1 - y0 == OL_TAIL && x + OL_TAIL <= lw) {
                *out++ = (u16)((s + 8) >> 4);   /* a whole cell */
                continue;
            }
            k = x + OL_TAIL <= lw ? OL_TAIL : lw - x;   /* a cell at the right or bottom edge: what it has */
            n = OL_MUL(k, y1 - y0);
            *out++ = (u16)((s + n / 2) / n);
        }
    }
}

/* The tail's Gaussian (sigma: tail pixels, Q8) on the plane pt, with the layer's work rows (acc, ring, tmp, the rows'
 * ranges: all free between the layer's Gaussians, and large enough for the smaller plane) */
static inline u32 ol_tail_blur(ol_t *o, u32 sigma)
{
    ol_t t;                                 /* what ol_gauss uses (a structure's copy would call memcpy) */
    u32 r;

    t.lw = o->tw;
    t.lh = o->th;
    t.rng = o->rng;
    t.acc = o->acc;
    t.ring = o->ring;
    t.tmp = o->tmp;
    t.pace = o->pace;
    t.ctx = o->ctx;
    t.done = t.all = 0;
    r = ol_gauss(&t, o->pt, sigma);
    o->done += t.done;
    o->all += t.all;
    return r;
}

/* The bilinear weights (x8) of OL_TAIL = 4: layer pixel 4j + q lies at j + (q - 1.5) / 4 of the tail's cell centres,
 * between cells j - 1 and j (q 0, 1: weights 3:5, 1:7) or j and j + 1 (q 2, 3: 7:1, 5:3); the edges held. Along a
 * column of the tail: the two rows and their weights of a layer row (ol_tail_up); along a row: all four at once */

/* A layer row's values of the tail's columns (x8): v[i] = r0[i] w0 + r1[i] w1 (w0 + w1 = 8) */
static OL_KERNEL void ol_tail_col(u32 *v, u32 *end, const u16 *r0, const u16 *r1, u32 w0, u32 w1)
{
    do {
        *v++ = OL_MUL(*r0++, w0) + OL_MUL(*r1++, w1);
    } while (v != end);
}

/* One layer pixel of the tail (t: x64) mixed into the Bloom's blur: (pb (256 - w) + T w + 128) >> 8 */
#define OL_TAIL_MIX(p, t) (*(p) = (u16)((OL_MUL(*(p), iw) + OL_MUL(((t) + 32) >> 6, w) + 128) >> 8))

/* One layer row of the tail: v[0..tw) its values in the row (x8), interpolated along it (x64 in all), four pixels a
 * cell of the tail, mixed into pb (lw pixels; a last cell may be narrower). The small factors by the extension unit
 * too: the compiler makes 3 s or 5 b of shifts and adds into the CPU's slow mul */
static OL_KERNEL void ol_tail_row(u16 *pb, const u32 *v, u32 lw, u32 tw, u32 w)
{
    u32 iw = 256 - w, a, b, c, b5, b7, j;
    u16 *end = pb + lw;

    for (j = 0, a = v[0]; j < tw; j++, a = b) {
        b = v[j];
        c = j + 1 < tw ? v[j + 1] : b;
        b5 = OL_MUL(b, 5);
        b7 = b5 + b + b;
        OL_TAIL_MIX(pb, OL_MUL(a, 3) + b5);          /* 3a + 5b */
        if (++pb == end)
            break;
        OL_TAIL_MIX(pb, a + b7);                     /* a + 7b */
        if (++pb == end)
            break;
        OL_TAIL_MIX(pb, b7 + c);                     /* 7b + c */
        if (++pb == end)
            break;
        OL_TAIL_MIX(pb, b5 + OL_MUL(c, 3));          /* 5b + 3c */
        if (++pb == end)
            break;
    }
}

/* The tail enlarged back to the layer and mixed into the Bloom's blur pb with the weight w (Q8; o->acc: a row) */
static inline void ol_tail_up(ol_t *o, u32 w)
{
    u32 y, jy, qy, tw = o->tw, th = o->th;
    const u16 *b;
    u16 *row = o->pb;

    for (y = 0, jy = 0, qy = 0; y < o->lh; y++, row += o->lw) {
        b = o->pt + OL_MUL(jy, tw);
        if (qy < 2)                         /* rows jy - 1 and jy: 3:5, 1:7 */
            ol_tail_col(o->acc, o->acc + tw, jy ? b - tw : b, b, qy ? 1 : 3, qy ? 7 : 5);
        else                                /* rows jy and jy + 1: 7:1, 5:3 */
            ol_tail_col(o->acc, o->acc + tw, b, jy + 1 < th ? b + tw : b, qy == 2 ? 7 : 5, qy == 2 ? 1 : 3);
        ol_tail_row(row, o->acc, o->lw, tw, w);
        if (++qy == OL_TAIL) {
            qy = 0;
            jy++;
        }
    }
}

/* The sigma the worker blurs with, in layer pixels Q8: of a Gaussian of `sigma` pixels of the picture (width W) on a layer
 * lw wide whose reduction and enlargement smooth by `floor` pixels (Gaussians add up in variance), 0 - the transport
 * alone is that wide already */
static inline u32 ol_cpu_sigma(u32 sigma, u32 floor, u32 W, u32 lw)
{
    u32 v;

    if (sigma <= floor || !W)
        return 0;
    v = ol_isqrt(sigma * sigma - floor * floor);     /* pixels of the picture, sigma below 65536 */
    return v * lw / W * 256 + (v * lw % W) * 256 / W;
}

/* The smallest sigma (pixels of the picture) of the transport for a layer lw wide on a picture W wide: the camera's ART
 * Soft Focus gives sigma 70 px on 5184 with its 160-wide frame (a cell of 32.4 px; a point target on the camera) - 2.16
 * cells; a cell of another layer is taken to smooth alike (a guess until measured) */
static inline u32 ol_floor_sigma(u32 W, u32 lw)
{
    return lw ? (216 * W + 50 * lw) / (100 * lw) : 0;
}

static inline u32 ol_clamp8(s32 v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (u32)v;
}

/* The neutral word: nothing added (Y 0, Cb and Cr 128) */
static inline u32 ol_neutral(const ol_par_t *p)
{
    return 128u << (8 * p->pcb) | 128u << (8 * p->pcr);
}

/* The word's tables (after the curves are in o and before ol_word): Halation's weight by the darkness of a code,
 * 256 - dark lin / 4096; the chroma bytes of a halation's Y code hy (0..255: enc is monotonic); the neutral word */
static inline void ol_word_tables(ol_t *o, const ol_par_t *p)
{
    s32 hy;

    for (hy = 0; hy < 256; hy++) {
        o->wd[hy] = (u16)(256 - (p->dark * o->lin[hy] >> 12));
        o->wb[hy] = (u16)(256 - (p->bdark * o->lin[hy] >> 12));
        o->cw[hy] = ol_clamp8(128 + (hy * p->hcb >> 8)) << (8 * p->pcb) | ol_clamp8(128 + (hy * p->hcr >> 8)) << (8 * p->pcr);
    }
    o->neutral = ol_neutral(p);
    o->ys = 8 * p->py;
}

/* The layer word of a pixel with the blurs b (Bloom) and h (Halation) and its own Y code c (ol_word_tables made): the
 * added light in Y, the halation's colour in Cb and Cr. Each added light weighted by the darkness of the place (Bloom's
 * weight 256 - none - unless bdark: (v 256) >> 8 is v). The halation's code is what it adds over the light before it:
 * over Bloom's (the first version), or over the place's own (hfirst: its colour does not fade under a strong Bloom) */
static inline u32 ol_word_of(const ol_t *o, const ol_par_t *p, u32 b, u32 h, u32 c)
{
    u32 base = o->lin[c], t1, t2, e1, e2, ab, ah;

    ab = OL_MUL(OL_MUL(b, p->gb) >> 8, o->wb[c]) >> 8;
    ah = OL_MUL(OL_MUL(h, p->gh) >> 8, o->wd[c]) >> 8;
    if (p->hfirst) {                        /* Halation first: its code from the base */
        t1 = base + ah;
        ah = ab;                            /* the second light added */
    } else {
        t1 = base + ab;
    }
    if (t1 > OL_ONE)
        t1 = OL_ONE;
    t2 = t1 + ah;
    if (t2 > OL_ONE)
        t2 = OL_ONE;
    e1 = o->enc[t1];
    e2 = o->enc[t2];
    return (e2 - o->enc[base]) << o->ys | o->cw[p->hfirst ? e1 - o->enc[base] : e2 - e1];
}

/* The word of pixel i; nothing added - the neutral word (what ol_word_of gives then) */
static inline u32 ol_word(const ol_t *o, const ol_par_t *p, u32 i)
{
    return o->pb[i] | o->ph[i] ? ol_word_of(o, p, o->pb[i], o->ph[i], o->y[i]) : o->neutral;
}

static OL_KERNEL u32 ol_word_lit(const ol_t *o, const ol_par_t *p, u32 i)
{
    return ol_word_of(o, p, o->pb[i], o->ph[i], o->y[i]);
}

/* The first pixel from k on, below n, that gets something (pb or ph not 0); n - none. Two pixels a step where the planes
 * allow it (most of a layer gets nothing) */
static OL_KERNEL u32 ol_quiet(const u16 *pb, const u16 *ph, u32 k, u32 n)
{
    const u32 *wb, *wh, *we;

    if (k < n && (((uptr)(pb + k) | (uptr)(ph + k)) & 2)) {
        if (pb[k] | ph[k])
            return k;
        k++;
    }
    if (!(((uptr)(pb + k) | (uptr)(ph + k)) & 2)) {
        wb = (const u32 *)(pb + k);
        wh = (const u32 *)(ph + k);
        for (we = wb + (n - k) / 2; wb != we && !(*wb | *wh); wb++)
            wh++;
        k = (u32)((const u16 *)wb - pb);
    }
    while (k < n && !(pb[k] | ph[k]))
        k++;
    return k;
}

/* d .. e (e > d) set to v */
static OL_KERNEL void ol_fill32(volatile u32 *d, volatile u32 *e, u32 v)
{
    do {
        *d++ = v;
    } while (d != e);
}

/* n words of the layer from pixel i into row (each written once). Most of a layer gets nothing (no source near): those
 * words are the neutral one, in runs, without the word's arithmetic */
static OL_KERNEL void ol_words(const ol_t *o, const ol_par_t *p, u32 i, volatile u32 *row, u32 n)
{
    const u16 *pb = o->pb + i, *ph = o->ph + i;
    u32 k = 0, j;

    while (k < n) {
        j = ol_quiet(pb, ph, k, n);
        if (j > k)
            ol_fill32(row + k, row + j, o->neutral);
        if (j == n)
            break;
        row[j] = ol_word_lit(o, p, i + j);
        k = j + 1;
    }
}

/* The test pattern (substage L3): eight bands across the layer; in band k < 4 byte k is 0x60 over the neutral word,
 * bands 4..7 neutral. With the additive blend: the Y byte's band brighter, a chroma byte's band tinted (Cb or Cr 32
 * below neutral), the fourth byte's band unchanged - whatever the positions given were */
static inline u32 ol_pattern(const ol_par_t *p, u32 x, u32 lw)
{
    u32 k = x * 8 / lw, w = ol_neutral(p);

    if (k < 4)
        w = (w & ~(0xffu << (8 * k))) | 0x60u << (8 * k);
    return w;
}

/* The lane statistics of the layer as read (substage L1): per byte, min, max, sum, sum of squares >> 8 */
typedef struct {
    u32 n;
    u32 min[OL_LANES], max[OL_LANES], sum[OL_LANES], sq[OL_LANES];
} ol_lanes_t;

static inline void ol_lanes_init(ol_lanes_t *s)
{
    u32 k;

    s->n = 0;
    for (k = 0; k < OL_LANES; k++) {
        s->min[k] = 255;
        s->max[k] = s->sum[k] = s->sq[k] = 0;
    }
}

static inline void ol_lanes_add(ol_lanes_t *s, u32 w)
{
    u32 k, v;

    for (k = 0; k < OL_LANES; k++) {
        v = w >> (8 * k) & 0xff;
        if (v < s->min[k])
            s->min[k] = v;
        if (v > s->max[k])
            s->max[k] = v;
        s->sum[k] += v;
        s->sq[k] += v * v >> 8;
    }
    s->n++;
}

/* The byte positions guessed from the statistics: the fourth byte is the one with the smallest range, Y the one with
 * the largest spread of the rest, then Cb before Cr (as in the picture's 4:2:2 words). 0 - guessed; 1 - not enough
 * to tell (no pixels, or the spreads too close) */
static inline int ol_lane_order(const ol_lanes_t *s, u32 *py, u32 *pcb, u32 *pcr)
{
    u32 k, pad = 0, y = OL_LANES, var[OL_LANES], m, second = 0, best = 0, c[2], nc = 0;

    if (s->n < 16)
        return 1;
    for (k = 0; k < OL_LANES; k++) {
        m = s->sum[k] / s->n;
        var[k] = s->sq[k] / s->n * 256 > m * m ? s->sq[k] / s->n * 256 - m * m : 0;
        if (s->max[k] - s->min[k] < s->max[pad] - s->min[pad])
            pad = k;
    }
    for (k = 0; k < OL_LANES; k++) {
        if (k == pad)
            continue;
        if (y == OL_LANES || var[k] > best) {
            second = best;
            best = var[k];
            y = k;
        } else if (var[k] > second) {
            second = var[k];
        }
    }
    if (y == OL_LANES || best < 2 * second + 16)
        return 1;
    for (k = 0; k < OL_LANES; k++)
        if (k != pad && k != y)
            c[nc++] = k;
    *py = y;
    *pcb = c[0];
    *pcr = c[1];
    return 0;
}

#endif

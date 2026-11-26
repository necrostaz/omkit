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
 * - Bloom's own emission (emb): a diffusion filter glows from bright surfaces too, so Bloom may take fewer whites and a
 *   lower threshold than the halation, which keeps to lamps;
 * - Bloom weighted by the darkness of the place, as Halation (on a bright place a filter's glow hardly shows);
 * - Halation's code from the place's own light, Bloom after it: its colour does not fade under a strong Bloom;
 * - Bloom's tail: a wide Gaussian on a plane OL_TAIL times smaller each way, added to Bloom's blur (ol_tail_*);
 * - Bloom's haze (haze): a share of every pixel's light, not only of the light over the threshold, in the tail's sources
 *   - a diffusion filter scatters a part of all it passes, so bright surfaces glow into the dark around them and the
 *   shadows lift; it lights the whole layer (every word gets the word's arithmetic);
 * - the scene's darkness (ol_scene, ol_day): the layer's mean Y and its share at a code (200) weigh a day scene,
 *   whose gains the caller lowers (a blown pixel there is a bright surface, not a lamp);
 * - the words of a run of lit pixels in one loop (ol_words_lit): no call a pixel;
 * - the cover read (ol_cover_*): every row of cells from two rows of the picture, one pixel of a pair - the same sources
 *   from job to job, whatever the time (before it: rows in passes until a budget ran out, the rest from the layer's Y);
 * - the strength's curve after the blur (ol_curve; bloom_toe, bloom_max, halation_toe, halation_max): a toe that takes
 *   a weak glow away and a ceiling that holds a strong one (all 0 - the light as it was);
 * - the look's white (card170, ol_white): a look's tone curve may end below 255 (a film's print white: 204..255 among
 *   the presets), so the codes are read on its scale - c' = min(c, W) 255 / W - in lin[] and enc[] of the job, and the
 *   added code never takes a pixel past W (W 255: the tables as they were, value for value); W from the look's tables
 *   (core), else from the shelf of the layer's Y (ol_shelf);
 * - the halation's colour from its weak edge's to its strong one's by its code (hcb2, hcr2, hcode), and its core
 *   without colour (hcore: the colour's code weighted by the darkness of the place once more);
 * - Bloom's colour (card170, bcol; ol_colour_*): a diffusion filter's glow has the colour of the light it scatters, so
 *   Bloom adds chroma at its sources' chromaticity - the layer's own Cb, Cr (the ISP's reduction of the picture) weighted
 *   by Bloom's sources, on a plane of cells of 8 x 8 (card173; card170 - the tail's plane) and blurred there, a share of
 *   it a unit of Bloom's code (0 - neutral, as before).
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
#define OL_INLINE inline __attribute__((always_inline))   /* a part of a loop (-Os would call it a pixel) */

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
    u32 emb;                                /* Bloom's source: as em, its own (card168: a diffusion filter glows from
                                               bright surfaces too, not only from what the picture blew) */
    u32 em;                                 /* the source: 0 - the excess over the threshold; 1..OL_EM_MAX - the emission
                                               of a pixel whose blown light stands for em whites (ol_emission) */
    u32 bdark;                              /* Bloom weighted by darkness as Halation: Q8 */
    u32 hfirst;                             /* 1 - Halation's code (and so its colour) from the place's own light, Bloom
                                               added after it; 0 - Bloom first */
    u32 st, tmix;                           /* Bloom's tail: a Gaussian of st (pixels of the tail's plane, Q8) on the
                                               sources reduced OL_TAIL times (ol_tail_*), mixed in with the weight tmix
                                               (Q8; 0 - no tail) */
    u32 haze;                               /* Bloom's haze: the share of every pixel's light (not only over the
                                               threshold) in the tail's sources, Q12 (0 - none; with a tail only) - a
                                               diffusion filter scatters a part of all the light it passes */
    u32 btoe, bmax, htoe, hmax;             /* the strength's curve after the blur (ol_curve), Q12 of white: Bloom's
                                               and Halation's toe and ceiling (0 each - no curve: the light as it was) */
    s32 hcb2, hcr2;                         /* card170: the halation's colour at its weak edge, Q8 as hcb, hcr (those
                                               are its strong colour then) */
    u32 hcode;                              /* ... the halation's Y code from which it has the strong colour: from the
                                               edge's at 0, straight between (0 - one colour, hcb, hcr) */
    u32 hcore;                              /* 1 - the colour's code weighted by the darkness of the place once more (a
                                               lamp's core stays white); 0 - not */
    u32 bcol;                               /* card170: Bloom's colour - the share of its sources' chromaticity (Cb / Y,
                                               Cr / Y) in its chroma a unit of its Y code, Q8 (0 - neutral, as before) */
    u32 sc;                                 /* ... the sigma its colour is blurred by, pixels of the tail's plane, Q8 */
} ol_par_t;

#define OL_EM_MAX 64                        /* whites a blown pixel stands for, at most: em OL_ONE stays below 2^18 */
#define OL_TAIL 4                           /* the tail's plane: cells of OL_TAIL x OL_TAIL layer pixels */
#define OL_CQ 8                             /* Bloom's colour's plane: cells of OL_CQ x OL_CQ layer pixels */
#define OL_COVER 2                          /* the cover read: rows of the picture a row of cells, at 1/4 and 3/4 of it */
#define OL_MEANS 0x80                       /* nrow: the row of cells has its means already (ol_cover_cells) */
#define OL_CURVE 4096                       /* the strength's curve: entries, the added light 0..2 whites in steps of 2 */

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
    u32 *cq;                                /* 256: the same Cb | Cr << 16, each + 256 (Bloom's colour adds to it:
                                               ol_bloom_chroma) */
    u16 *wd;                                /* 256: Halation's weight by the darkness of the place, Q8, by its code */
    u16 *wb;                                /* 256: the same of Bloom */
    u16 *hz;                                /* 256: Bloom's haze of a code, haze lin >> 12 (ol_word_tables) */
    u32 hazed;                              /* 1 - hz[] not all 0: ol_tail_down adds it */
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
    u32 tabd;                               /* 1 - tab[] made for this job (ol_read_table): ol_sources does not again */
    u32 y0;                                 /* 1 - the sums are of one pixel a pair (Y0: the cover read), 0 - of both */
    u16 *kb, *kh;                           /* OL_CURVE each: the strength's curves of Bloom and Halation (ol_curve) */
    u32 bc, hc;                             /* 1 - the curve in use (ol_word_tables), 0 - the light as it was */
    u32 hk;                                 /* 1 - the colour's code weighted by wd[] (p->hcore) */
    u16 *wq;                                /* 256: the weight of a source's colour by its code, Q8 (ol_word_tables) */
    u16 *pq;                                /* Bloom's colour (ol_colour_*): 4 planes qw x qh of the sources' sums of S Cb,
                                               S Cr, S Y, S, then qc[] (qw x qh u32); at least 2 lw + 768 values (scratch
                                               before the read's end) */
    u32 qw, qh;                             /* Bloom's colour's plane: the layer's size / OL_CQ rounded up, qw then one
                                               more and even (its last columns copies of the last cell) */
    u32 bq;                                 /* 1 - Bloom's colour in the words (qc[] made; ol_colour_row before a row) */
    u32 crow;                               /* the first pixel of the row ol_colour_row made (o->acc its colours) */
    u32 cymin;                              /* ... the least Bloom's code that its colour can change somewhere (256 - none) */
    u8 *cym;                                /* ... the same at each column of cells (qw: after its colours in acc) */
    u32 cpair;                              /* ... its two rows of cells a | b << 16 (cym[] theirs; ~0 - none yet) */
    u32 cqk;                                /* Bloom's colour's sums: their means x 2^cqk (ol_colour_down) */
    u32 scb, scr;                           /* the shifts of Cb and Cr in a word (8 pcb, 8 pcr) */
} ol_t;

/* The work buffer's parts for a layer lw x lh from address b (4-aligned): the pointers set in o (lin and enc too: the
 * caller copies the curves there); the end address. With b 0 and a scratch o: the bytes needed */
static inline uptr ol_layout(ol_t *o, uptr b, u32 lw, u32 lh)
{
    u32 n = (lw * lh + 1) & ~1u;            /* the planes stay 4-aligned */

    o->lw = lw;
    o->lh = lh;
    o->tw = ((lw + OL_TAIL - 1) / OL_TAIL + 1) & ~1u;   /* even: the tail's column passes go two columns a step */
    o->th = (lh + OL_TAIL - 1) / OL_TAIL;
    o->qw = ((lw + OL_CQ - 1) / OL_CQ + 2) & ~1u;     /* even (paired column passes), past the last cell one more */
    o->qh = (lh + OL_CQ - 1) / OL_CQ;
    o->tabd = 0;
    o->lin = (const u16 *)b;
    b += 512;
    o->enc = (const u8 *)b;
    b += OL_ENC;
    o->tab = (u32 *)b;
    b += 1024;
    o->cw = (u32 *)b;
    b += 1024;
    o->cq = (u32 *)b;
    b += 1024;
    o->wd = (u16 *)b;
    b += 512;
    o->wb = (u16 *)b;
    b += 512;
    o->hz = (u16 *)b;
    b += 512;
    o->kb = (u16 *)b;
    b += 2 * OL_CURVE;
    o->kh = (u16 *)b;
    b += 2 * OL_CURVE;
    o->wq = (u16 *)b;
    b += 512;
    o->pq = (u16 *)b;
    b += 2 * (7 * o->qw * o->qh + 2 * o->qh > 2 * lw + 768 ? 7 * o->qw * o->qh + 2 * o->qh : 2 * lw + 768);   /* the four
                                               sums, qc[], the largest colours (ol_colour_ratio) */
    o->pt = (u16 *)b;
    b += (2 * o->tw * o->th + 3) & ~3u;
    o->acc = (u32 *)b;
    o->cym = (u8 *)(o->acc + o->qw + 1);    /* (qw + 1) 4 + qw bytes: within lw words */
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

/* The look's white (card170): the job's curves lin[], enc[] on the scale of a look whose white is the code W (its tone
 * curve's output for the largest input; OL_WHITE_MIN..255) from the curves lin0[] of ol_luts. A code c stands for c' =
 * min(c, W) 255 / W: lin[c] = lin0 at c' (Q8, between its entries), OL_ONE at W and above (a sharpening's overshoot past
 * the look's white is white); enc[v] - the largest code at most W whose lin[] is at most v. So the sources, the weights by
 * darkness and the light added (enc[base + added] - enc[base]) are the same on every look for the same picture on its
 * own scale, and a pixel never gets past W. W 255 (or out of range): lin0 and its enc exactly (ol_luts's) */
#define OL_WHITE_MIN 160                    /* the lowest white taken (the presets' 204..255; below - a fault) */
static inline void ol_white(u16 *lin, u8 *enc, const u16 *lin0, u32 W)
{
    u32 c, x, i, v, k;

    if (W > 255 || W < OL_WHITE_MIN)
        W = 255;
    for (c = 0; c < 256; c++) {
        if (c >= W) {
            lin[c] = OL_ONE;
            continue;
        }
        x = c * 255 * 256 / W;              /* c' in Q8, below 255 x 256 */
        i = x >> 8;
        lin[c] = (u16)(lin0[i] + (((u32)(lin0[i + 1] - lin0[i]) * (x & 255) + 128) >> 8));
    }
    for (v = 0, k = 0; v < OL_ENC; v++) {
        while (k < W && lin[k + 1] <= v)
            k++;
        enc[v] = (u8)k;
    }
}

/* The look's white when its tables are not known: the shelf of the layer's Y - a blown part of the picture is a run of
 * pixels at the look's white, a peak at the top of the histogram. Every other row (as ol_scene), into hist[256]: from the
 * top, the first pair of codes (c - 1, c) holding at least OL_SHELF_PM permille of the pixels gives the shelf when it
 * also holds OL_SHELF_PEAK times the mean pair of the eight codes below it (a smooth tail of the highlights is no shelf):
 * the code of the pair with more pixels. 0 - none at lo or above: nothing blown, the picture's white not known (the caller
 * keeps 255 - the codes as they are). A picture without a blown part could give a shelf below the real white (the
 * brightest flat surface): hence the peak's test and lo */
#define OL_SHELF_PM 3                       /* 0.3 % of the pixels */
#define OL_SHELF_PEAK 3
static OL_KERNEL u32 ol_shelf(const ol_t *o, u32 *hist, u32 lo)
{
    u32 y, n = 0, c, s, b, k, lw = o->lw;
    const u8 *p, *e;

    for (c = 0; c < 256; c++)
        hist[c] = 0;
    for (y = 0; y < o->lh; y += 2) {
        for (p = o->y + OL_MUL(y, lw), e = p + lw; p != e; p++)
            hist[*p]++;
        n += lw;
    }
    if (lo < 10)
        lo = 10;
    for (c = 255; c >= lo; c--) {
        s = hist[c] + hist[c - 1];
        if (s * 1000 < n * OL_SHELF_PM)
            continue;
        for (b = 0, k = 2; k < 10; k++)     /* the 8 codes below the pair: 4 pairs */
            b += hist[c - k];
        if (s * 4 < OL_SHELF_PEAK * b)
            return 0;
        return hist[c] >= hist[c - 1] ? c : c - 1;
    }
    return 0;
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
 * the threshold, or the emission (Bloom's p->emb, Halation's p->em). A half holds the sum of 16 pixels (16 OL_ONE < 65536), so the read adds at
 * most 8 words before it splits the halves */
static inline void ol_read_table(ol_t *o, const ol_par_t *p)
{
    u32 i, l, eb, eh, m = p->em > OL_EM_MAX ? OL_EM_MAX : p->em, mb = p->emb > OL_EM_MAX ? OL_EM_MAX : p->emb;

    for (i = 0; i < 256; i++) {
        l = o->lin[i];
        eb = mb ? ol_emission(l, p->tb, mb) : l > p->tb ? l - p->tb : 0;
        eh = m ? ol_emission(l, p->th, m) : l > p->th ? l - p->th : 0;
        o->tab[i] = eb | eh << 16;
    }
    o->tabd = 1;
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
    if (o->nrow[cy] < OL_MEANS - 1)
        o->nrow[cy]++;
}

/* The cover read (card169): every row of cells read the same way, whatever the time - OL_COVER rows of the picture a row
 * of cells, at 1/4 and 3/4 of its height (16 rows: 4 and 12; the midpoints of its two halves, so the samples are 8 rows
 * apart over the whole picture and a cell's slope does not bias its mean), one pixel of each pair (Y0: every other
 * column - half the table's loads; the cache lines are the same). The read of a row of cells is the same from job to job:
 * a thin bright line counts by its share of the cell's samples (1 of 2 rows, not 1 of the 1 row read before, which
 * made a wire's glint a cell's whole source), and no row of cells falls back on the layer's own Y (only on an error or a
 * stop). The sums: 16 samples a cell of 8 pairs - each half at most 16 OL_ONE, below 65536 */

/* The picture's row of the sample k (0..OL_COVER - 1) of the row of cells cy (after ol_rows; H the picture's height) */
static inline u32 ol_cover_y(const ol_t *o, u32 cy, u32 k, u32 H)
{
    u32 y0 = o->ry[cy], y1 = cy + 1 < o->lh ? o->ry[cy + 1] : H;

    return y0 + (2 * k + 1) * (y1 - y0) / (2 * OL_COVER);
}

/* The Y0 of the words of a row [row, end) through the read's table: the sums of Bloom | Halation << 16 */
static OL_KERNEL u32 ol_read_run0(const volatile u32 *row, const volatile u32 *end, const u32 *tab)
{
    u32 s = 0, w;

    do {
        w = *row++;
        s += OL_TABW(tab, (w & 0xff) << 2);
    } while (row != end);
    return s;
}

/* ol_read_pack's cover read: out[cx] = the Y0 sums of a cell of OL_READ_RUN words (Bloom | Halation << 16), added to
 * out[cx] when add (the second row of a row of cells), else stored; bytes as ol_read_pack's. On the camera an unrolled
 * loop of 4 (bytes) or 5 (words) instructions a word; drv_optics_layer checks it against ol_read_run0 at start */
#ifdef __mn10300__
#define OL_PACK0(first, load, mask)                                                                                         \
    "1:\n\t" first "\n\t.irp k,0,8,16,24\n\t" load " (\\k,%3),%1\n\t" load " (\\k+4,%3),%2\n\t"                        \
    "asl2 %1\n\tasl2 %2\n\t" mask "mov (%1,%5),%1\n\tmov (%2,%5),%2\n\tadd %1,%0\n\tadd %2,%0\n\t.endr\n\t"            \
    "add 32,%3\n\tmov %0,(%4)\n\tinc4 %4\n\tcmp %6,%4\n\tbne 1b"
#define OL_PACK0_ASM(str)                                                                                                   \
    __asm__ __volatile__(str : "=&d"(s), "=&d"(a), "=&d"(b), "+a"(row), "+a"(out) : "a"(tab), "a"(end) : "cc", "memory")
#endif
static OL_KERNEL void ol_read_pack0(const volatile u32 *row, u32 *out, const u32 *end, const u32 *tab, u32 bytes,
                                    u32 add)
{
#ifdef __mn10300__
    u32 s, a, b;

    if (bytes && add)
        OL_PACK0_ASM(OL_PACK0("mov (%4),%0", "movbu", ""));
    else if (bytes)
        OL_PACK0_ASM(OL_PACK0("clr %0", "movbu", ""));
    else if (add)
        OL_PACK0_ASM(OL_PACK0("mov (%4),%0", "mov", "and 1020,%1\n\tand 1020,%2\n\t"));
    else
        OL_PACK0_ASM(OL_PACK0("clr %0", "mov", "and 1020,%1\n\tand 1020,%2\n\t"));
#else
    (void)bytes;
    do {
        *out = (add ? *out : 0) + ol_read_run0(row, row + OL_READ_RUN, tab);
        out++;
        row += OL_READ_RUN;
    } while (out != end);
#endif
}

/* The packed sums of 16 samples into the means of a row of cells: floor(sum / 16) of each half (what ol_means_row
 * gives for them) */
static OL_KERNEL void ol_read_split16(const u32 *s, u16 *db, u16 *dh, const u16 *de)
{
    do {
        u32 v = *s++;

        *db++ = (u16)((v & 0xffff) >> 4);
        *dh++ = (u16)(v >> 20);
    } while (db != de);
}

/* The sample k of the row of cells cy: the picture's row `row` (ol_cover_y's) into its sums. Cells of OL_READ_RUN words
 * and no shift (a layer 324 wide on 5184): the row's sums packed in o->acc (not in use while the picture is read), the
 * means made at the last sample (OL_MEANS) - so a row of cells' samples are read one after the other; a stop between
 * them leaves the row of cells unread (the layer's Y). Else: added to the sums (Y0, >> shift), the means by ol_sources */
static inline void ol_cover_cells(ol_t *o, const volatile u32 *row, u32 cy, u32 k)
{
    u32 lw = o->lw, n, j, s, b, h;
    u16 *db = o->pb + OL_MUL(cy, lw), *dh = o->ph + OL_MUL(cy, lw);
    const u8 *c = o->cwp, *ce = c + lw;

    o->y0 = 1;
    if (o->c8 && !o->shift) {
        ol_read_pack0(row, o->acc, o->acc + lw, o->tab, o->rbytes, k);
        if (k == OL_COVER - 1) {
            ol_read_split16(o->acc, db, dh, db + lw);
            o->nrow[cy] = OL_COVER | OL_MEANS;
        }
        return;
    }
    do {
        n = *c++;
        b = h = 0;
        do {
            j = n > 2 * OL_READ_RUN ? 2 * OL_READ_RUN : n;   /* 16 samples: each half below 65536 */
            n -= j;
            s = ol_read_run0(row, row + j, o->tab);
            row += j;
            b += s & 0xffff;
            h += s >> 16;
        } while (n);
        *db = (u16)(*db + (b >> o->shift));
        *dh = (u16)(*dh + (h >> o->shift));
        db++;
        dh++;
    } while (c != ce);
    if (o->nrow[cy] < OL_MEANS - 1)
        o->nrow[cy]++;
}

/* The means of a row of cells of n samples a pair (2 k: both pixels of k rows; k: Y0 of k rows): floor((x << shift) /
 * d), d = cwp n, as the high word of (x << shift) m with m = 0xffffffff / d + 1 - exact while (x << shift) d < 2^32 - and
 * both factors below 2^31 for ol_mulhi (d >= 4: see ol_sources); the reciprocal made again only where the column's pairs
 * change (a layer 324 wide: never) */
static OL_KERNEL void ol_means_row(u16 *pb, u16 *ph, const u8 *cwp, const u8 *ce, u32 n, u32 shift)
{
    u32 c = 0, m = 0;

    do {
        if (*cwp != c) {
            c = *cwp;
            m = 0xffffffffu / (c * n) + 1;
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
    u32 cx, cy, i, t, d, k, cm = 0, cn = 255, recip, lw = o->lw, spp = o->y0 ? 1 : 2;
    u16 *pb = o->pb, *ph = o->ph;
    const u8 *y;

    if (!o->tabd)
        ol_read_table(o, p);
    for (cx = 0; cx < lw; cx++) {
        if (o->cwp[cx] > cm)
            cm = o->cwp[cx];
        if (o->cwp[cx] < cn)
            cn = o->cwp[cx];
    }
    recip = o->shift < 16 && spp * cn >= 4 && (65535u << o->shift) < 0xffffffffu / (2 * cm * 255);
    for (cy = 0, i = 0; cy < o->lh; cy++, i += lw) {
        k = o->nrow[cy];
        if (k & OL_MEANS) {
            continue;                       /* the cover read made them */
        } else if (k && recip) {
            ol_means_row(pb + i, ph + i, o->cwp, o->cwp + lw, spp * k, o->shift);
        } else if (k) {
            for (cx = 0; cx < lw; cx++) {
                d = spp * (u32)o->cwp[cx] * k;
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
static OL_KERNEL void ol_box_cols(ol_t *o, u16 *plane, u32 r, u32 ia, u32 ib)
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
 * Gaussian there (ol_tail_blur), then enlarged back bilinearly and added to the Bloom's blur: pb = G(pb) + f T with
 * f = w / (256 - w) (ol_tail_add) - the caller scales Bloom's gain by (256 - w) / 256, so the light is (256 - w) G + w T
 * as a mix would give, but a row the tail does not reach costs nothing. The mean and the bilinear enlargement keep the
 * light's sum. The plane is one column wider when the layer's cells are odd (an even width: paired column passes), that
 * column a copy of the last. */

/* The haze of a row of the layer's Y codes y .. y + lw into the sums acc[] of its tail cells (ol_tail_down) */
static OL_KERNEL void ol_tail_haze(u32 *acc, const u8 *y, u32 lw, const u16 *hz)
{
    u32 x;

    for (x = 0; x + OL_TAIL <= lw; x += OL_TAIL, y += OL_TAIL)
        *acc++ += (u32)hz[y[0]] + hz[y[1]] + hz[y[2]] + hz[y[3]];
    for (; x < lw; x++)
        *acc += hz[*y++];
}

/* The tail's sources: pt[cell] = the mean of the Bloom's sources over the cell (o->acc: the sums of a row of cells),
 * with Bloom's haze (o->hazed: the haze of the layer's own Y, ol_word_tables's hz) - then at most OL_ONE */
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
            if (o->hazed)
                ol_tail_haze(acc, o->y + OL_MUL(y, lw), lw, o->hz);
        }
        for (tx = 0, x = 0; tx < tw; tx++, x += OL_TAIL) {
            s = acc[tx];
            if (x >= lw) {                  /* the column that makes the width even: a copy of the last */
                *out = out[-1];
                out++;
                continue;
            }
            if (y1 - y0 == OL_TAIL && x + OL_TAIL <= lw) {
                s = (s + 8) >> 4;           /* a whole cell */
            } else {
                k = x + OL_TAIL <= lw ? OL_TAIL : lw - x;   /* a cell at the right or bottom edge: what it has */
                n = OL_MUL(k, y1 - y0);
                s = (s + n / 2) / n;
            }
            *out++ = (u16)(s > OL_ONE ? OL_ONE : s);   /* the haze's mean at most OL_ONE (without it the sources' mean
                                                          is): f OL_ONE stays below 65536 (ol_tail_add) */
        }
    }
}

/* A Gaussian (sigma: the plane's pixels, Q8) on a small plane w x h (the tail's, Bloom's colour's), with the layer's work
 * rows (acc, ring, tmp, the rows' ranges: all free between the layer's Gaussians, and large enough for the smaller
 * plane) */
static inline u32 ol_plane_blur_wh(ol_t *o, u16 *plane, u32 w, u32 h, u32 sigma)
{
    ol_t t;                                 /* what ol_gauss uses (a structure's copy would call memcpy) */
    u32 r;

    t.lw = w;
    t.lh = h;
    t.rng = o->rng;
    t.acc = o->acc;
    t.ring = o->ring;
    t.tmp = o->tmp;
    t.pace = o->pace;
    t.ctx = o->ctx;
    t.done = t.all = 0;
    r = ol_gauss(&t, plane, sigma);
    o->done += t.done;
    o->all += t.all;
    return r;
}

/* The tail's Gaussian on its plane */
static inline u32 ol_plane_blur(ol_t *o, u16 *plane, u32 sigma)
{
    return ol_plane_blur_wh(o, plane, o->tw, o->th, sigma);
}

static inline u32 ol_tail_blur(ol_t *o, u32 sigma)
{
    return ol_plane_blur(o, o->pt, sigma);
}

/* Bloom's colour (card170; on its own plane since card173). A diffusion filter scatters the light it passes: the glow
 * round a warm lamp is warm. The colour of a source is the layer's own chroma there (its words' Cb and Cr: the ISP's
 * reduction of the picture, 16 x 16 pixels a cell; pc holds them, Cb | Cr << 8, from the layer's read until Bloom's
 * Gaussian), weighted by Bloom's source S of the pixel and by wq[] of its code (a blown core is white in the picture
 * whatever the lamp's colour: its colour counts 1/8). Made on a plane of cells of OL_CQ x OL_CQ layer pixels (qw x qh),
 * four values a cell side by side (A, B, Y, D: ol_gauss4 blurs the four at once) - the sums S' Cb / 256, S' Cr / 256,
 * S' Y / 256, S' (S' = S wq / 256; Cb, Cr the bytes, 128 neutral; each term truncated a pixel), their means (at most
 * OL_ONE: the Gaussian's paired columns), blurred alike (ol_colour_blur), then the chromaticity a cell
 * (ol_colour_ratio): (sum S' (Cb - 128)) / (sum S' Y) - the chroma a unit of Y of the light that glows there - times the
 * share, into qc[] (Cb | Cr << 16, 128 + Q7). The words take it where Bloom adds a code (ol_words_lit): the cells'
 * colours interpolated bilinearly at the pixel (ol_colour_row along the row's column once a row, ol_colour_at along the
 * row), Bloom's code times it added to the chroma (ol_bloom_chroma).
 * card170 made it on the tail's plane (cells of 4 x 4), four Gaussians one by one, and enlarged it into a plane of the
 * layer: ~10 million instructions a job with the words' share (~0.1-0.18 s on the camera, card171). The colour is a
 * smooth field (blurred by the tail's sigma, ~25 layer pixels), so: cells of 8 x 8 (a quarter of the values), one
 * Gaussian for the four, and the colour only where a word takes it */
#define OL_COL_SOFT 16                      /* added to a cell's sum S' Y in the ratio: where little light glows the colour
                                               fades to neutral smoothly (no edge where the sums end) */
#define OL_QN 0x00800080u                   /* a neutral colour in qc[] and ol_colour_at: Cb 128 | Cr 128 << 16 */
#define OL_CQ_SMAX (8 * 256)                /* the colour's sigma at most (its plane's pixels, Q8; ~1000 px of the
                                               picture): a box of radius 7 at most - ol_gauss4's paired sums */

/* The lit pixels (pb not 0) of n pixels into the sums s[0..3]: S' Cb / 256, S' Cr / 256, S' Y / 256, S' */
static OL_KERNEL void ol_colour_px(u32 *s, const u16 *pb, const u8 *y, const u16 *pc, const u16 *wq, u32 n)
{
    do {
        u32 b = *pb++;

        if (b) {
            u32 c = *y, q = *pc, v = OL_MUL(b, wq[c]) >> 8;

            s[3] += v;
            s[2] += OL_MUL(v, c) >> 8;
            s[0] += OL_MUL(v, q & 0xff) >> 8;
            s[1] += OL_MUL(v, q >> 8) >> 8;
        }
        y++;
        pc++;
    } while (--n);
}

/* 1 - the cell of OL_CQ x h pixels from pixel k has a source (pb not 0), its four sums into s; 0 - none. Its words
 * looked at first (the layer's width even: its rows 4-aligned; most of a layer has no source), then the rows with one */
static OL_KERNEL u32 ol_colour_cell(const ol_t *o, u32 *s, u32 k, u32 h)
{
    const u32 *w = (const u32 *)(o->pb + k), *e = w;
    u32 m = 0, lw = o->lw, lw2 = lw / 2, j;

    for (j = h; j; j--, e += lw2)
        m |= e[0] | e[1] | e[2] | e[3];
    if (!m)
        return 0;
    s[0] = s[1] = s[2] = s[3] = 0;
    for (j = h; j; j--, w += lw2, k += lw)
        if (w[0] | w[1] | w[2] | w[3])
            ol_colour_px(s, o->pb + k, o->y + k, o->pc + k, o->wq, OL_CQ);
    return 1;
}

/* The plane of the sums' means (qw x qh cells of four values at o->pq) x 2^cqk; before Bloom's Gaussian (pc: the
 * layer's chroma). The scale: the most the largest stays within OL_ONE (the Gaussian's paired columns), 1..16 - the
 * Gaussian rounds each pass to a unit, and the colour of a faint glow (a small lamp's, far from its source: a few units
 * of Y and less of 2 A - D) is the ratio of such small numbers; with 1 (the largest at OL_ONE or above) the means
 * clamped as card170's. First the means x 16 (at most 65535), then the scale */
#define OL_CQ_K 4
static OL_KERNEL void ol_colour_down(ol_t *o)
{
    u32 lw = o->lw, lh = o->lh, x, y0, h, w, j, n, s[4], k, xe = OL_CQ * o->qw, m = 0, sh;
    u16 *q = o->pq, *e;

    for (y0 = 0; y0 < lh; y0 += OL_CQ) {
        h = lh - y0 < OL_CQ ? lh - y0 : OL_CQ;
        k = OL_MUL(y0, lw);
        for (x = 0; x < xe; x += OL_CQ, q += 4) {
            if (x >= lw) {                  /* the columns that make the width even and one more: copies of the last */
                ((u32 *)q)[0] = ((const u32 *)q)[-2];
                ((u32 *)q)[1] = ((const u32 *)q)[-1];
                continue;
            }
            w = lw - x < OL_CQ ? lw - x : OL_CQ;
            if (w == OL_CQ && !(lw & 1)) {
                if (!ol_colour_cell(o, s, k + x, h)) {
                    ((u32 *)q)[0] = ((u32 *)q)[1] = 0;
                    continue;
                }
            } else {
                s[0] = s[1] = s[2] = s[3] = 0;
                for (j = 0, n = k + x; j < h; j++, n += lw)
                    ol_colour_px(s, o->pb + n, o->y + n, o->pc + n, o->wq, w);
            }
            n = w * h;
            for (j = 0; j < 4; j++) {
                if (n == OL_CQ * OL_CQ)
                    s[j] = (s[j] + 2) >> 2;     /* the mean x 16 */
                else
                    s[j] = (s[j] * 16 + n / 2) / n;
                s[j] = s[j] > 65535 ? 65535 : s[j];
                m = s[j] > m ? s[j] : m;
                q[j] = (u16)s[j];
            }
        }
    }
    for (sh = 0; sh < OL_CQ_K && (m + (1u << sh >> 1)) >> sh > OL_ONE; sh++)
        ;
    o->cqk = OL_CQ_K - sh;
    for (q = o->pq, e = q + 4 * o->qw * o->qh; q != e; q++) {
        k = sh ? (*q + (1u << (sh - 1))) >> sh : *q;
        *q = (u16)(k > OL_ONE ? OL_ONE : k);
    }
}

/* One extended box along a row of n cells of packed words (two values a word, the halves; a word every other one: two
 * words a cell) from v, the cells [s, e) of it made: ol_box_row's arithmetic in each half (its sums at most 15 x 4096:
 * no carry) */
static OL_KERNEL void ol_box_rowp(u32 *v, u32 n, u32 s, u32 e, u32 r, u32 ia, u32 ib, u32 *P)
{
    u32 i, sum, lp = s ? 0 : v[0], rp = e < n ? 0 : v[2 * (n - 1)], *out = v + 2 * s, *end = v + 2 * e;
    const u32 *lo, *hi;

    for (i = 0; i <= r; i++) {
        P[i] = lp;
        P[r + 1 + e - s + i] = rp;
    }
    for (i = s, lo = out; i < e; i++, lo += 2)
        P[r + 1 + i - s] = *lo;
    for (sum = 0, i = 1; i <= 2 * r + 1; i++)
        sum += P[i];
    lo = P;                                 /* P[i]: the value r + 1 before; P[i + 2r + 2]: r + 1 after */
    hi = P + 2 * r + 2;
    do {
        u32 l = *lo++, h = *hi++, t = l + h;

        *out = (OL_MUL(sum & 0xffff, ia) + OL_MUL(t & 0xffff, ib) + 32768) >> 16 |
               (OL_MUL(sum >> 16, ia) + OL_MUL(t >> 16, ib) + 32768) >> 16 << 16;
        out += 2;
        sum += h - *lo;
    } while (out != end);
}

/* The Gaussian of sigma (the plane's pixels, Q8; at most OL_CQ_SMAX) over a plane of w x h cells of four values (a u16
 * plane 4 w wide): each value as ol_gauss would give it on its own plane, all four in one pass - the rows by packed words
 * (ol_box_rowp), the columns as ol_box_cols's paired columns (a column pass does not mix columns). 0, or 1 - stopped */
static inline u32 ol_gauss4(ol_t *o, u16 *plane, u32 w, u32 h, u32 sigma)
{
    ol_t t;                                 /* what the passes use (a structure's copy would call memcpy) */
    u32 r, a, ia, ib, k, y, s, e, stop = 0;
    u16 *rg, *row;

    if (sigma < 64)
        return 0;
    if (sigma > OL_CQ_SMAX)
        sigma = OL_CQ_SMAX;
    t.lw = 4 * w;
    t.lh = h;
    t.rng = o->rng;
    t.acc = o->acc;
    t.ring = o->ring;
    t.done = 0;
    ol_box_plan(sigma, &r, &a);
    ol_box_weights(r, a, &ia, &ib);
    ol_ranges(&t, plane);
    o->all += 2 * OL_BOX_PASSES * 4 * w * h;
    for (k = 0; k < OL_BOX_PASSES && !stop; k++) {
        for (y = 0, rg = o->rng, row = plane; y < h; y++, rg += 2, row += 4 * w) {
            if (rg[0] >= rg[1])
                continue;                   /* a row of 0 stays 0 */
            s = rg[0] / 4;                  /* the range in cells, widened by r + 1 */
            e = (rg[1] + 3) / 4;
            s = s > r + 1 ? s - r - 1 : 0;
            e = e + r + 1 < w ? e + r + 1 : w;
            ol_box_rowp((u32 *)row, w, s, e, r, ia, ib, (u32 *)o->tmp);
            ol_box_rowp((u32 *)row + 1, w, s, e, r, ia, ib, (u32 *)o->tmp);
            rg[0] = (u16)(4 * s);
            rg[1] = (u16)(4 * e);
            t.done += 4 * (e - s);
        }
        stop = o->pace && o->pace(o->ctx, OL_ROWS);
        if (!stop) {
            ol_box_cols(&t, plane, r, ia, ib);
            stop = o->pace && o->pace(o->ctx, OL_COLS);
        }
    }
    o->done += t.done;
    return stop;
}

/* The sums blurred by the sigma sc (the tail's pixels - OL_TAIL layer pixels - Q8; OL_CQ / OL_TAIL times fewer in the
 * colour's plane). 0, or 1 - stopped */
static inline u32 ol_colour_blur(ol_t *o, u32 sc)
{
    return ol_gauss4(o, o->pq, o->qw, o->qh, (sc * OL_TAIL + OL_CQ / 2) / OL_CQ);
}

/* A cell's chromaticity times the share s (Q8): (2 A - D) s / (4 (Y + OL_COL_SOFT 2^cqk)) - about (sum S'(Cb - 128) / sum S' Y)
 * s 128 / 256 in Q7 (A = sum S' Cb / 256, D = sum S', Y = sum S' Y / 256), clamped to +-127; into qc[] (after the
 * plane) as 128 + it, Cb | Cr << 16. After qc[] the largest |Cb - 128|, |Cr - 128| of each row of cells (u16) and of
 * each cell (u8): ol_colour_row's bounds. The products by OL_MUL: their low word is the signed product's */
static OL_KERNEL void ol_colour_ratio(ol_t *o, u32 s)
{
    u32 n = o->qw * o->qh, x, m, v;
    const u16 *q = o->pq;
    u32 *qc = (u32 *)(o->pq + 4 * n), *e = qc + n;
    u16 *rd = (u16 *)e;
    u8 *cd = (u8 *)(rd + o->qh);
    s32 a, b, d, y;

    o->cpair = ~0u;                         /* ol_colour_row's bounds made again */
    do {
        for (x = o->qw, m = 0; x; x--) {
            d = q[3];
            y = 4 * (q[2] + (OL_COL_SOFT << o->cqk));
            a = (s32)OL_MUL(2 * (s32)q[0] - d, s) / y;
            b = (s32)OL_MUL(2 * (s32)q[1] - d, s) / y;
            a = a < -127 ? -127 : a > 127 ? 127 : a;
            b = b < -127 ? -127 : b > 127 ? 127 : b;
            *qc++ = (u32)(128 + a) | (u32)(128 + b) << 16;
            a = a < 0 ? -a : a;
            b = b < 0 ? -b : b;
            v = (u32)(a > b ? a : b);
            *cd++ = (u8)v;
            m = v > m ? v : m;
            q += 4;
        }
        *rd++ = (u16)m;
    } while (qc != e);
}

/* ceil(64 / d), d 1..127 (0: 255): the least Bloom's code yb that a colour d from 128 can change - with yb d < 64,
 * (yb (q - 128) + 64) >> 7 is 0 */
static const u8 ol_cym64[128] = {
    255, 64, 32, 22, 16, 13, 11, 10, 8, 8, 7, 6, 6, 5, 5, 5, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

/* Bilinear between the cells' centres (pixel OL_CQ j + 3.5 of a row or column): pixel OL_CQ j + r lies between cells j - 1
 * and j (r 0..3: weights 7:9, 5:11, 3:13, 1:15 in 16ths) or j and j + 1 (r 4..7: 15:1, 13:3, 11:5, 9:7); the edges held */
static const u8 ol_cq_w0[OL_CQ] = {7, 5, 3, 1, 15, 13, 11, 9};

/* Layer row y's colours at the cells' columns, interpolated along the column (x16), into o->acc[1..qw] with acc[0] =
 * acc[1] (cell -1 held), and o->crow its first pixel: ol_colour_at's row (once a row, before the row's words). And the
 * least Bloom's code its colour can change: a colour of the row's cell j is between those of the row's two rows of
 * cells and of the columns j - 1 .. j + 1, at most d from 128 (ol_colour_ratio's largest), so a code yb with yb d < 64
 * changes no word there - o->cym[j] = ceil(64 / d), o->cymin the row's least; a row without colour (d 0) is not
 * interpolated (cymin 256) */
static OL_KERNEL void ol_colour_row(ol_t *o, u32 y)
{
    u32 qw = o->qw, r = y & (OL_CQ - 1), j = y / OL_CQ, a, b, w0, d, prev, cur, next;
    const u32 *ra, *rb, *qc = (const u32 *)(o->pq + 4 * qw * o->qh);
    const u16 *rd = (const u16 *)(qc + qw * o->qh);
    const u8 *da, *db;
    u32 *c = o->acc + 1, *e = c + qw;
    u8 *cm = o->cym;

    w0 = ol_cq_w0[r];
    if (r < OL_CQ / 2) {
        a = j ? j - 1 : 0;
        b = j;
    } else {
        a = j;
        b = j + 1 < o->qh ? j + 1 : j;
    }
    d = rd[a] > rd[b] ? rd[a] : rd[b];
    o->crow = OL_MUL(y, o->lw);
    o->cymin = d ? ol_cym64[d] : 256;
    if (!d)
        return;
    if (o->cpair != (a | b << 16)) {        /* the bounds of a pair of rows of cells: once for its 8 layer rows */
        o->cpair = a | b << 16;
        da = (const u8 *)(rd + o->qh) + OL_MUL(a, qw);
        db = (const u8 *)(rd + o->qh) + OL_MUL(b, qw);
        prev = cur = *da > *db ? *da : *db;
        for (j = 1; j <= qw; j++, prev = cur, cur = next) {
            next = j < qw ? (da[j] > db[j] ? da[j] : db[j]) : cur;
            d = prev > cur ? prev : cur;
            d = next > d ? next : d;
            *cm++ = ol_cym64[d];
        }
    }
    ra = qc + OL_MUL(a, qw);
    rb = qc + OL_MUL(b, qw);
    do {
        *c++ = (*rb << 4) + OL_MUL(*ra - *rb, w0);  /* ra w0 + rb w1: each half at most 255 x 16 (ol_colour_at) */
        ra++;
        rb++;
    } while (c != e);
    o->acc[0] = o->acc[1];
}

/* The colour at pixel x of the row ol_colour_row made: Cb | Cr << 16, each 128 + Q7 (rounded; each half at most
 * 255 x 256 + 128: no carry). The packed words' difference may borrow between the halves: the sum is the true one all
 * the same (each true half in 0..65535) */
static OL_INLINE u32 ol_colour_at(const ol_t *o, u32 x)
{
    u32 r = x & (OL_CQ - 1), w0 = ol_cq_w0[r];
    const u32 *c = o->acc + x / OL_CQ + (r >= OL_CQ / 2);

    return ((c[1] << 4) + OL_MUL(c[0] - c[1], w0) + 0x00800080u) >> 8 & 0x00ff00ffu;   /* c0 w0 + c1 (16 - w0) */
}

/* The bilinear weights (x8) of OL_TAIL = 4: layer pixel 4j + q lies at j + (q - 1.5) / 4 of the tail's cell centres,
 * between cells j - 1 and j (q 0, 1: weights 3:5, 1:7) or j and j + 1 (q 2, 3: 7:1, 5:3); the edges held. Along a
 * column of the tail: the two rows and their weights of a layer row (ol_tail_add); along a row: all four at once */

/* A layer row's values of the tail's columns [a, b) (x8): v[i] = r0[i] w0 + r1[i] w1 (w0 + w1 = 8) */
static OL_KERNEL void ol_tail_col(u32 *v, u32 *end, const u16 *r0, const u16 *r1, u32 w0, u32 w1)
{
    do {
        *v++ = OL_MUL(*r0++, w0) + OL_MUL(*r1++, w1);
    } while (v != end);
}

/* One layer row of the tail added to pb: v[0..tn) its values (x8; tn the cells of the layer, the last may be narrower),
 * interpolated along the row (x64 in all), four pixels a cell, from cell j0 to j1 (the rest gets 0 there) */
static OL_KERNEL void ol_tail_row(u16 *pb, const u32 *v, u32 lw, u32 tn, u32 j0, u32 j1)
{
    u32 a, b, c, b5, b7, j;
    u16 *end = pb + lw;

    pb += OL_TAIL * j0;
    for (j = j0, a = v[j0 ? j0 - 1 : 0]; j < j1; j++, a = b) {
        b = v[j];
        c = j + 1 < tn ? v[j + 1] : b;
        b5 = (b << 2) + b;
        b7 = b5 + b + b;
        *pb += (u16)(((a << 1) + a + b5 + 32) >> 6);     /* 3a + 5b */
        if (++pb == end)
            break;
        *pb += (u16)((a + b7 + 32) >> 6);                /* a + 7b */
        if (++pb == end)
            break;
        *pb += (u16)((b7 + c + 32) >> 6);                /* 7b + c */
        if (++pb == end)
            break;
        *pb += (u16)((b5 + (c << 1) + c + 32) >> 6);     /* 5b + 3c */
        if (++pb == end)
            break;
    }
}

/* The tail enlarged back to the layer and added to the Bloom's blur pb at the factor f (Q8: w / (256 - w) of the weight
 * w; up to 9 x 256 - pb stays below 65536). The plane is scaled by f first, each of its rows' non-zero range noted
 * (o->rng: free between the Gaussians); a layer row whose two tail rows are 0 is left as it is (o->acc: a row) */
static inline void ol_tail_add(ol_t *o, u32 f)
{
    u32 y, jy, qy, tw = o->tw, th = o->th, tn = (o->lw + OL_TAIL - 1) / OL_TAIL, i, x0, x1, a, b, j0, j1;
    u16 *row = o->pb, *t = o->pt, *rg = o->rng;
    const u16 *r0, *r1;

    for (y = 0; y < th; y++, t += tw) {
        for (i = 0, x0 = tn, x1 = 0; i < tn; i++) {
            t[i] = (u16)((OL_MUL(t[i], f) + 128) >> 8);
            if (t[i]) {
                if (x0 == tn)
                    x0 = i;
                x1 = i + 1;
            }
        }
        rg[2 * y] = (u16)x0;
        rg[2 * y + 1] = (u16)x1;
    }
    for (y = 0, jy = 0, qy = 0; y < o->lh; y++, row += o->lw) {
        if (qy < 2) {                       /* rows jy - 1 and jy: 3:5, 1:7 */
            a = jy ? jy - 1 : 0;
            b = jy;
        } else {                            /* rows jy and jy + 1: 7:1, 5:3 */
            a = jy;
            b = jy + 1 < th ? jy + 1 : jy;
        }
        x0 = rg[2 * a] < rg[2 * b] ? rg[2 * a] : rg[2 * b];
        x1 = rg[2 * a + 1] > rg[2 * b + 1] ? rg[2 * a + 1] : rg[2 * b + 1];
        if (x0 < x1) {                      /* the cells' values on [x0, x1), 0 around it; the pixels of the cells one
                                               wider each way (the interpolation's reach) */
            j0 = x0 ? x0 - 1 : 0;
            j1 = x1 + 1 < tn ? x1 + 1 : tn;
            r0 = o->pt + OL_MUL(a, tw);
            r1 = o->pt + OL_MUL(b, tw);
            if (j0)
                o->acc[j0 - 1] = 0;
            for (i = j0; i < x0; i++)
                o->acc[i] = 0;
            for (i = x1; i < j1 + 1 && i < tw; i++)
                o->acc[i] = 0;
            if (qy < 2)
                ol_tail_col(o->acc + x0, o->acc + x1, r0 + x0, r1 + x0, qy ? 1 : 3, qy ? 7 : 5);
            else
                ol_tail_col(o->acc + x0, o->acc + x1, r0 + x0, r1 + x0, qy == 2 ? 7 : 5, qy == 2 ? 1 : 3);
            ol_tail_row(row, o->acc, o->lw, tn, j0, j1);
        }
        if (++qy == OL_TAIL) {
            qy = 0;
            jy++;
        }
    }
}

/* The factor of ol_tail_add for the tail's weight w (Q8, below 256) */
static inline u32 ol_tail_factor(u32 w)
{
    return w ? (w * 256 + (256 - w) / 2) / (256 - w) : 0;
}

/* The scene's darkness from the layer's Y (every other row): its mean code and the share (permille) of its pixels at
 * the code t or above */
static OL_KERNEL void ol_scene(const ol_t *o, u32 t, u32 *mean, u32 *share)
{
    u32 y, n = 0, sum = 0, hi = 0, lw = o->lw;
    const u8 *p, *e;

    for (y = 0; y < o->lh; y += 2) {
        for (p = o->y + OL_MUL(y, lw), e = p + lw; p != e; p++) {
            sum += *p;
            hi += *p >= t;
        }
        n += lw;
    }
    *mean = n ? (sum + n / 2) / n : 0;
    *share = n ? (hi * 1000 + n / 2) / n : 0;
}

/* The day's weight 0..256 of a scene: 0 at or below a, 256 at or above b, straight between - by the mean (d[0], d[1])
 * and by the share (d[2], d[3]); the larger one */
static inline u32 ol_ramp(u32 v, u32 a, u32 b)
{
    return v <= a ? 0 : v >= b ? 256 : (v - a) * 256 / (b - a);
}

static inline u32 ol_day(u32 mean, u32 share, const u32 *d)
{
    u32 m = ol_ramp(mean, d[0], d[1]), s = ol_ramp(share, d[2], d[3]);

    return m > s ? m : s;
}

/* A gain scaled by the day's weight: at day 256 it is pct percent of itself */
static inline u32 ol_day_gain(u32 g, u32 day, u32 pct)
{
    u32 k = 256 - (day * (256 - pct * 256 / 100) >> 8);  /* Q8 */

    return (g * k + 128) >> 8;
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

/* The strength's curve after the blur (card169): the added light x (Q12, before the weight by darkness) of entry i = x / 2
 * (0..2 whites) into
 *   g = x^2 / (x + toe)          - the toe: a weak glow (x well below toe) fades, a strong one loses about toe;
 *   v = max g / (g + max)        - the ceiling: v is g while g is well below max, never above max;
 * toe 0 - g is x; max 0 - v is g. The slope is 1 at the toe's end and below the ceiling, so the gains keep their sense.
 * A blown wire's glint lifts a cell a little - the toe takes it away; a lamp's core is held under max, so its halo has
 * no flat disc with an edge (the red channel at 255). Q12 throughout: x^2 < 2^26, max g < 2^26 */
static inline void ol_curve(u16 *t, u32 toe, u32 max)
{
    u32 i, x, g, v;

    for (i = 0; i < OL_CURVE; i++) {
        x = 2 * i;
        g = toe ? x * x / (x + toe) : x;
        v = max ? max * g / (g + max) : g;
        t[i] = (u16)v;
    }
}

/* A light through a curve (o->kb or o->kh): its entry, the last one past 2 whites */
static inline u32 ol_curved(const u16 *t, u32 x)
{
    return t[x >= 2 * OL_CURVE ? OL_CURVE - 1 : x >> 1];
}

/* The chroma (Q8 a code) of a halation's code hy between its edge's colour a (at 0) and its strong one b (from k on):
 * hy (a + (b - a) min(hy, k) / k) */
static inline s32 ol_hue(s32 hy, s32 a, s32 b, u32 k)
{
    s32 m = hy < (s32)k ? hy : (s32)k;

    return hy * a + hy * (b - a) * m / (s32)k;
}

/* The word's tables (after the curves are in o and before ol_word, ol_tail_down): Halation's weight by the darkness of
 * a code, 256 - dark lin / 4096 (and Bloom's); the chroma bytes of a halation's Y code hy (0..255: enc is monotonic);
 * Bloom's haze of a code (haze lin / 4096); the strength's curves (none when their toe and max are 0); the neutral word */
static inline void ol_word_tables(ol_t *o, const ol_par_t *p)
{
    s32 hy;

    for (hy = 0; hy < 256; hy++) {
        o->wd[hy] = (u16)(256 - (p->dark * o->lin[hy] >> 12));
        o->wb[hy] = (u16)(256 - (p->bdark * o->lin[hy] >> 12));
        o->hz[hy] = (u16)(OL_MUL(p->haze > 4096 ? 4096 : p->haze, o->lin[hy]) >> 12);
        if (p->hcode)                       /* the edge's colour at 0, the strong one from hcode on (card170) */
            o->cw[hy] = ol_clamp8(128 + (ol_hue(hy, p->hcb2, p->hcb, p->hcode) >> 8)) << (8 * p->pcb) |
                        ol_clamp8(128 + (ol_hue(hy, p->hcr2, p->hcr, p->hcode) >> 8)) << (8 * p->pcr);
        else
            o->cw[hy] = ol_clamp8(128 + (hy * p->hcb >> 8)) << (8 * p->pcb) | ol_clamp8(128 + (hy * p->hcr >> 8)) <<
                        (8 * p->pcr);
        o->cq[hy] = ((o->cw[hy] >> (8 * p->pcb) & 0xff) | (o->cw[hy] >> (8 * p->pcr) & 0xff) << 16) + 0x01000100u;
    }
    o->hk = p->hcore;
    o->bq = p->bcol != 0;
    for (hy = 0; hy < 256; hy++)            /* a source's colour by its code: a blown core (white in the picture) at 1/8 */
        o->wq[hy] = (u16)(256 - (224 * (u32)o->lin[hy] >> 12));
    o->hazed = p->haze && p->tmix;
    o->bc = p->btoe || p->bmax;
    o->hc = p->htoe || p->hmax;
    if (o->bc)
        ol_curve(o->kb, p->btoe, p->bmax);
    if (o->hc)
        ol_curve(o->kh, p->htoe, p->hmax);
    o->neutral = ol_neutral(p);
    o->ys = 8 * p->py;
    o->scb = 8 * p->pcb;
    o->scr = 8 * p->pcr;
}

/* The layer word of a pixel with the blurs b (Bloom) and h (Halation) and its own Y code c (ol_word_tables made): the
 * added light in Y, the halation's colour in Cb and Cr. Each added light through its strength's curve when it has one
 * (ol_curve), then weighted by the darkness of the place (Bloom's weight 256 - none - unless bdark: (v 256) >> 8 is v).
 * The halation's code is what it adds over the light before it: over Bloom's (the first version), or over the place's
 * own (hfirst: its colour does not fade under a strong Bloom). ol_word_parts: the word's Y byte (returned), the index of
 * its chroma in cw[] and cq[] (*hy: the halation's code, weighted for its colour) and Bloom's code (*yb: what Bloom
 * adds to Y) */
static OL_INLINE u32 ol_word_parts(const ol_t *o, const ol_par_t *p, u32 b, u32 h, u32 c, u32 *hy, u32 *yb)
{
    u32 base = o->lin[c], t1, t2, e0, e1, e2, ab, ah;

    ab = OL_MUL(b, p->gb) >> 8;
    ah = OL_MUL(h, p->gh) >> 8;
    if (o->bc)
        ab = ol_curved(o->kb, ab);
    if (o->hc)
        ah = ol_curved(o->kh, ah);
    ab = OL_MUL(ab, o->wb[c]) >> 8;
    ah = OL_MUL(ah, o->wd[c]) >> 8;
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
    e0 = o->enc[base];
    e1 = o->enc[t1];
    e2 = o->enc[t2];
    if (p->hfirst) {                        /* the halation's code; Bloom's: the second light's, or the first's */
        t1 = e1 - e0;
        *yb = e2 - e1;
    } else {
        t1 = e2 - e1;
        *yb = e1 - e0;
    }
    if (o->hk)                              /* its colour weighted by darkness once more: a lamp's core stays white */
        t1 = OL_MUL(t1, o->wd[c]) >> 8;
    *hy = t1;
    return (e2 - e0) << o->ys;
}

/* The chroma bytes of a word with Bloom's colour (card170): the halation's chroma cq[hy] (Cb | Cr << 16), plus Bloom's
 * code yb times the colour q of the place (Cb | Cr << 16, each 128 + Q7 chroma a unit of Y: ol_colour_at), rounded,
 * clamped, at the bytes' positions. Cb + ((yb (q - 128) + 64) >> 7) is taken as ((yb q + 64) >> 7) - yb (the same: 128 yb
 * is a multiple of 128) for both halves at once: one product (yb q + 64 below 65536: no carry), the halves biased by 256
 * in cq[] (each 1..1019 then); a half outside 256..511 is clamped */
static OL_INLINE u32 ol_bloom_chroma(const ol_t *o, u32 hy, u32 yb, u32 q)
{
    u32 v = o->cq[hy] + ((OL_MUL(yb, q) + 0x00400040u) >> 7 & 0x01ff01ffu) - (yb | yb << 16);

    if ((v & 0xff00ff00u) != 0x01000100u)
        return ol_clamp8((s32)(v & 0xffff) - 256) << o->scb | ol_clamp8((s32)(v >> 16) - 256) << o->scr;
    return (v & 0xff) << o->scb | (v >> 16 & 0xff) << o->scr;
}

/* The word with Bloom's colour q of the place (o->bq; OL_QN - none) */
static inline u32 ol_word_q(const ol_t *o, const ol_par_t *p, u32 b, u32 h, u32 c, u32 q)
{
    u32 hy, yb, y = ol_word_parts(o, p, b, h, c, &hy, &yb);

    return y | (o->bq && yb && q != OL_QN ? ol_bloom_chroma(o, hy, yb, q) : o->cw[hy]);
}

static inline u32 ol_word_of(const ol_t *o, const ol_par_t *p, u32 b, u32 h, u32 c)
{
    return ol_word_q(o, p, b, h, c, OL_QN);
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

/* The chroma bytes of a lit pixel at x of the row ol_colour_row made, where Bloom adds a code (hb: the halation's
 * code hy | Bloom's code yb << 16, yb not 0): the halation's chroma with Bloom's colour there (ol_colour_at,
 * ol_bloom_chroma; a neutral colour adds nothing). Out of the words' loop: their few registers stay with the word's
 * arithmetic */
static OL_KERNEL u32 ol_word_colour(const ol_t *o, u32 x, u32 hb)
{
    u32 q = ol_colour_at(o, x);

    return q == OL_QN ? o->cw[hb & 0xffff] : ol_bloom_chroma(o, hb & 0xffff, hb >> 16, q);
}

/* The word of pixel i (as ol_words gives it); nothing added - the neutral word (what ol_word_of gives then). With
 * Bloom's colour: a pixel of the row ol_colour_row made */
static inline u32 ol_word(const ol_t *o, const ol_par_t *p, u32 i)
{
    u32 hy, yb, w;

    if (!(o->pb[i] | o->ph[i]))
        return o->neutral;
    w = ol_word_parts(o, p, o->pb[i], o->ph[i], o->y[i], &hy, &yb);
    return w | (o->bq && yb >= o->cymin && yb >= o->cym[(i - o->crow) / OL_CQ] ? ol_word_colour(o, i - o->crow, hy |
                yb << 16) : o->cw[hy]);
}

/* The words of a run of lit pixels from k (pb or ph not 0 at k) into row, until a pixel with nothing or n: the next
 * pixel. One loop, the word's arithmetic inline (no call a pixel: with Bloom's tail most of a layer is lit). With Bloom's
 * colour (o->bq: the row's colours made, ol_colour_row) a pixel where Bloom adds a code its colour may change (from
 * o->cymin and its column's o->cym on) takes the colour there (ol_word_colour), *nc counts those */
static OL_KERNEL u32 ol_words_lit(const ol_t *o, const ol_par_t *p, u32 i, volatile u32 *row, u32 k, u32 n, u32 *nc)
{
    const u16 *pb = o->pb + i, *ph = o->ph + i;
    const u8 *y = o->y + i;
    u32 hy, yb, w, x0, c = 0;

    if (o->bq) {
        x0 = i - o->crow;
        do {
            w = ol_word_parts(o, p, pb[k], ph[k], y[k], &hy, &yb);
            if (yb >= o->cymin && yb >= o->cym[(x0 + k) / OL_CQ]) {
                w |= ol_word_colour(o, x0 + k, hy | yb << 16);
                c++;
            } else {
                w |= o->cw[hy];
            }
            row[k] = w;
        } while (++k < n && (pb[k] | ph[k]));
        *nc += c;
        return k;
    }
    do {
        w = ol_word_parts(o, p, pb[k], ph[k], y[k], &hy, &yb);
        row[k] = w | o->cw[hy];
    } while (++k < n && (pb[k] | ph[k]));
    return k;
}

/* n words of the layer from pixel i into row (each written once). Most of a layer gets nothing (no source near): those
 * words are the neutral one, in runs, without the word's arithmetic. With Bloom's colour the pixels are of the row
 * ol_colour_row made. Returns the pixels that took Bloom's colour */
static OL_KERNEL u32 ol_words(const ol_t *o, const ol_par_t *p, u32 i, volatile u32 *row, u32 n)
{
    const u16 *pb = o->pb + i, *ph = o->ph + i;
    u32 k = 0, j, nc = 0;

    while (k < n) {
        j = ol_quiet(pb, ph, k, n);
        if (j > k)
            ol_fill32(row + k, row + j, o->neutral);
        if (j == n)
            break;
        k = ol_words_lit(o, p, i, row, j, n, &nc);
    }
    return nc;
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

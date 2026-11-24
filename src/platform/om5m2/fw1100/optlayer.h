/* The optics layer's plain parts (opticsdrv.c's worker task): Bloom and Halation drawn into the small layer that the ISP
 * then enlarges and adds to the picture. Only plain C, no firmware address or call: the host test
 * (tests/host/test_optlayer.c) runs the very code. The includer defines u8, u16, s32, u32 (omk_types.h).
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
 *
 * The layer's word (format 0x305, 4 bytes a pixel): the byte of Y, Cb and Cr by ol_par_t's positions (the hardware's
 * order is not known - the camera's dump decides, ol_lane_order); the fourth byte 0.
 *
 * The Gaussian (ol_gauss): separable, rows then columns, each a cascade of OL_BOX_PASSES extended boxes - a box of
 * radius r with the next value on either side weighted alpha (Gwosdek, Grewenig, Bruhn, Weickert, "Theoretical
 * foundations of Gaussian convolution by extended box filtering", SSVM 2011): the variance of the cascade is exactly
 * the one asked for (alpha takes the rest), the shape is the Gaussian's by the central limit, and the cost a value is
 * the same whatever the sigma. Sigma here is in layer pixels, Q8; the worker takes off what the ISP's reduction and enlargement
 * smooth anyway (ol_cpu_sigma).
 *
 * No division in the inner loops (a reciprocal a pass), no table but the two in the caller's buffer. */
#ifndef OMK_OPTLAYER_H
#define OMK_OPTLAYER_H

#define OL_ONE 4095                         /* linear light Q12: a code of 255 */
#define OL_ENC 4096                         /* entries of enc[] */
#define OL_LANES 4

typedef struct {
    u32 tb, th;                             /* thresholds, linear Q12 */
    u32 sb1, sb2, mixb;                     /* Bloom's Gaussians: sigmas in layer pixels Q8 (0 - none), the second's
                                               weight Q8 (0 - one Gaussian) */
    u32 sh1, sh2, mixh;                     /* the same for Halation */
    u32 gb, gh;                             /* gains of the added light, Q8 (256 - the excess once) */
    u32 dark;                               /* Halation weighted by darkness: Q8, 0 - not at all, 256 - none on white */
    s32 hcb, hcr;                           /* Halation's chroma a unit of its Y code, Q8 signed */
    u32 py, pcb, pcr;                       /* the bytes (0..3) of Y, Cb and Cr in a layer word */
} ol_par_t;

typedef struct {
    u32 lw, lh;                             /* the layer */
    u8 *y;                                  /* lw * lh: the layer's Y as read */
    u16 *pb, *ph;                           /* lw * lh: the sources of Bloom and Halation, then their blurs */
    u16 *pc;                                /* lw * lh: the second Gaussian's copy */
    u16 *tmp;                               /* max(lw, lh) + 1 */
    u8 *nrow;                               /* lh: rows of the picture read into each row of cells */
    u8 *cwp;                                /* lw: pixel pairs of the picture in each column of cells */
    const u16 *lin;                         /* 256 */
    const u8 *enc;                          /* OL_ENC */
    u32 shift;                              /* the read's sums are kept >> shift (u16) */
} ol_t;

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

/* The pairs of picture pixels in each column of cells (width W, even, layer lw): cwp[cx]; 1 - a column has none or more
 * than 255 (the layer too small for the picture: not used) */
static inline int ol_columns(u8 *cwp, u32 W, u32 lw)
{
    u32 p, cx = 0, acc = 0, n = 0;

    for (p = 0; p < W / 2; p++) {
        n++;
        acc += 2 * lw;                      /* the pair's right edge in cells x W */
        if (acc >= (cx + 1) * W) {
            if (n > 255)
                return 1;
            cwp[cx++] = (u8)n;
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

/* The bit reversal of 4 bits: the order of a read's passes, each one halving the gaps the ones before left */
static inline u32 ol_bitrev4(u32 j)
{
    return (j & 1) << 3 | (j & 2) << 1 | (j & 4) >> 1 | (j & 8) >> 3;
}

/* One row of the picture (4:2:2, words Y0 Cb Y1 Cr) into the sums of the row of cells cy: per cell column, the
 * excess over each threshold of its pixels, >> shift. The read itself is the cost (past the cache: ~0.3 us a word) */
static inline void ol_read_row(ol_t *o, const ol_par_t *p, const volatile u32 *row, u32 cy)
{
    u32 cx, k, n, w, l0, l1, sb, sh, tb = p->tb, th = p->th;
    u16 *db = o->pb + cy * o->lw, *dh = o->ph + cy * o->lw;
    const u16 *lin = o->lin;

    for (cx = 0; cx < o->lw; cx++) {
        sb = sh = 0;
        for (k = 0, n = o->cwp[cx]; k < n; k++) {
            w = *row++;
            l0 = lin[w & 0xff];
            l1 = lin[w >> 16 & 0xff];
            sb += (l0 > tb ? l0 - tb : 0) + (l1 > tb ? l1 - tb : 0);
            sh += (l0 > th ? l0 - th : 0) + (l1 > th ? l1 - th : 0);
        }
        db[cx] = (u16)(db[cx] + (sb >> o->shift));
        dh[cx] = (u16)(dh[cx] + (sh >> o->shift));
    }
    if (o->nrow[cy] < 255)
        o->nrow[cy]++;
}

/* After the read (o->pb, o->ph hold sums where nrow > 0, anything elsewhere): the mean excess of a pixel, Q12, in every
 * cell - from the sums where the picture's rows were read, from the layer's Y elsewhere */
static inline void ol_sources(ol_t *o, const ol_par_t *p)
{
    u32 cx, cy, i, l, d;

    for (cy = 0, i = 0; cy < o->lh; cy++)
        for (cx = 0; cx < o->lw; cx++, i++) {
            if (o->nrow[cy]) {
                d = 2 * (u32)o->cwp[cx] * o->nrow[cy];
                o->pb[i] = (u16)(((u32)o->pb[i] << o->shift) / d);
                o->ph[i] = (u16)(((u32)o->ph[i] << o->shift) / d);
            } else {
                l = o->lin[o->y[i]];
                o->pb[i] = (u16)(l > p->tb ? l - p->tb : 0);
                o->ph[i] = (u16)(l > p->th ? l - p->th : 0);
            }
        }
}

#define OL_BOX_PASSES 3
#define OL_SIGMA_MAX (32 * 256)             /* sigma in layer pixels, Q8: the plan's arithmetic stays in 32 bits */

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

/* One extended box over n values at v[0], v[step], ... (edges clamped), through tmp[n]. The sum of the box S and of its
 * two ends E: (S + a E / 256) / (2r + 1 + 2a / 256), as S ia + E ib >> 16 (values up to OL_ONE, r below 64: in 32 bits) */
static inline void ol_box(u16 *v, u32 n, u32 step, u32 r, u32 alpha, u16 *tmp)
{
    u32 i, sum = 0, w = (2 * r + 1) * 256 + 2 * alpha, ia = (65536u * 256 + w / 2) / w, ib = alpha * ia >> 8, e;
    s32 k;

    for (i = 0; i < n; i++)
        tmp[i] = v[i * step];
    for (k = -(s32)r; k <= (s32)r; k++)
        sum += tmp[k < 0 ? 0 : (u32)k < n ? (u32)k : n - 1];
    for (i = 0; i < n; i++) {
        e = tmp[i >= r + 1 ? i - r - 1 : 0] + tmp[i + r + 1 < n ? i + r + 1 : n - 1];
        v[i * step] = (u16)((sum * ia + e * ib + 32768) >> 16);
        sum += tmp[i + r + 1 < n ? i + r + 1 : n - 1];
        sum -= tmp[i >= r ? i - r : 0];
    }
}

/* The Gaussian of sigma (layer pixels, Q8) over a plane, rows then columns; sigma below 0.25 pixel: nothing */
static inline void ol_gauss(ol_t *o, u16 *plane, u32 sigma)
{
    u32 r, a, x, y, k;

    if (sigma < 64)
        return;
    if (sigma > OL_SIGMA_MAX)
        sigma = OL_SIGMA_MAX;
    ol_box_plan(sigma, &r, &a);
    for (k = 0; k < OL_BOX_PASSES; k++) {
        for (y = 0; y < o->lh; y++)
            ol_box(plane + y * o->lw, o->lw, 1, r, a, o->tmp);
        for (x = 0; x < o->lw; x++)
            ol_box(plane + x, o->lh, o->lw, r, a, o->tmp);
    }
}

/* A source blurred by one Gaussian (s2 or mix 0) or by (256 - mix) G(s1) + mix G(s2), the copy through o->pc */
static inline void ol_blur2(ol_t *o, u16 *plane, u32 s1, u32 s2, u32 mix)
{
    u32 i, n = o->lw * o->lh;

    if (!mix || !s2) {
        ol_gauss(o, plane, s1);
        return;
    }
    if (mix > 256)
        mix = 256;
    for (i = 0; i < n; i++)
        o->pc[i] = plane[i];
    ol_gauss(o, plane, s1);
    ol_gauss(o, o->pc, s2);
    for (i = 0; i < n; i++)
        plane[i] = (u16)(((u32)plane[i] * (256 - mix) + (u32)o->pc[i] * mix + 128) >> 8);
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

/* The layer word of pixel i after the blurs: the added light in Y, the halation's colour in Cb and Cr */
static inline u32 ol_word(const ol_t *o, const ol_par_t *p, u32 i)
{
    u32 base = o->lin[o->y[i]], b, h, wd, t1, t2, e0, e1, e2;
    s32 hy;

    b = (u32)o->pb[i] * p->gb >> 8;
    wd = 256 - (p->dark * base >> 12);
    h = ((u32)o->ph[i] * p->gh >> 8) * wd >> 8;
    t1 = base + b;
    if (t1 > OL_ONE)
        t1 = OL_ONE;
    t2 = t1 + h;
    if (t2 > OL_ONE)
        t2 = OL_ONE;
    e0 = o->enc[base];
    e1 = o->enc[t1];
    e2 = o->enc[t2];
    hy = (s32)(e2 - e1);
    return (e2 - e0) << (8 * p->py) | ol_clamp8(128 + (hy * p->hcb >> 8)) << (8 * p->pcb) |
           ol_clamp8(128 + (hy * p->hcr >> 8)) << (8 * p->pcr);
}

/* The neutral word: nothing added (Y 0, Cb and Cr 128) */
static inline u32 ol_neutral(const ol_par_t *p)
{
    return 128u << (8 * p->pcb) | 128u << (8 * p->pcr);
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

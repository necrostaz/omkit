/* Host test of the optics layer's plain parts (src/platform/om5m2/fw1100/optlayer.h, the code the worker task runs): the
 * curves, the columns of cells, the read of the picture's rows into the sources (against a direct computation), the blur
 * (a constant stays, the mass stays, it spreads, its sigma - also of two Gaussians - the one asked for), the words
 * (nothing added under the thresholds, light added and coloured above them, no change of a white place), the pattern
 * and the guess of the byte positions. Then the fast code against the first version (optlayer_ref.h): the same sums of
 * the read, the same means, the same Gaussians (row and column passes, the rows' ranges, the paired columns, every
 * sigma, edges, sparse and dense planes) and the same words. Prints "ok", or what failed (with an argument: also the
 * comparisons' numbers). */
#include <stdio.h>
#include <string.h>
#include "omk_types.h"
#include "optlayer.h"
#include "optlayer_ref.h"

static int failed, verbose;              /* verbose (any argument): the comparisons' numbers too */

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "%s\n", what);
        failed = 1;
    }
}

static u16 lin[256];
static u8 enc[OL_ENC];
static u32 wbuf[(1 << 20) / 4];             /* the work buffer of ol_layout */

static u32 seed = 12345;

static u32 rnd(u32 n)
{
    seed = seed * 1103515245u + 12345u;
    return (seed >> 8) % n;
}

/* o over the work buffer for a layer lw x lh, the curves copied in */
static void layout(ol_t *o, u32 lw, u32 lh)
{
    uptr end;

    memset(o, 0, sizeof(*o));
    end = ol_layout(o, (uptr)wbuf, lw, lh);
    if (end > (uptr)wbuf + sizeof(wbuf)) {
        check(0, "layout: the test's buffer too small");
        return;
    }
    memcpy((void *)o->lin, lin, sizeof(lin));
    memcpy((void *)o->enc, enc, sizeof(enc));
}

static void curves(void)
{
    u32 c, v;
    int mono = 1, inv = 1;

    ol_luts(220, lin, enc);
    check(lin[0] == 0 && lin[255] == OL_ONE, "lin: 0 and 255 at the ends");
    for (c = 1; c < 256; c++)
        mono &= lin[c] >= lin[c - 1];
    check(mono, "lin: monotonic");
    check(lin[128] > 800 && lin[128] < 1000, "lin: the middle code about 0.22 of the light (gamma 2.2)");
    for (c = 0; c < 256; c++)
        inv &= enc[lin[c]] == c || lin[c] == lin[enc[lin[c]]];
    check(inv, "enc: the inverse of lin");
    for (v = 1, mono = 1; v < OL_ENC; v++)
        mono &= enc[v] >= enc[v - 1];
    check(mono && enc[OL_ENC - 1] == 255, "enc: monotonic, 255 at the end");
    ol_luts(150, lin, enc);
    check(lin[128] == 128 * 128 * OL_ONE / 65025 || lin[128] == 128 * 128 * OL_ONE / 65025 + 1, "gamma below 200: x^2");
    ol_luts(220, lin, enc);
}

static void columns(void)
{
    u8 cwp[400];
    u32 i, sum;

    check(!ol_columns(cwp, 5184, 324), "5184 / 324: columns made");
    for (i = 0, sum = 0; i < 324; i++)
        sum += cwp[i];
    check(sum == 2592, "5184 / 324: every pair in a column");
    check(cwp[0] == 8 && cwp[323] == 8, "5184 / 324: 8 pairs a column");
    check(!ol_columns(cwp, 5184, 250), "5184 / 250: columns made");
    for (i = 0, sum = 0; i < 250; i++)
        sum += cwp[i];
    check(sum == 2592, "5184 / 250: every pair in a column");
    check(ol_columns(cwp, 64, 40), "64 / 40: refused (columns of no pair)");
    check(ol_read_shift(cwp, 250, 16) >= 1, "the read's shift keeps the sums in 16 bits");
}

#define W 64
#define H 48
#define LW 8
#define LH 6

static u32 pic[H][W / 2];

static void params(ol_par_t *p)
{
    memset(p, 0, sizeof(*p));
    p->tb = lin[200];
    p->th = lin[230];
    p->gb = p->gh = 256;
    p->dark = 0;
    p->hcb = -64;
    p->hcr = 128;
    p->py = 0;
    p->pcb = 1;
    p->pcr = 2;
}

static void reads(void)
{
    ol_t o;
    ol_par_t p;
    u32 x, y, cy, cx, i, e, want;
    int same = 1;

    layout(&o, LW, LH);
    params(&p);
    check(!ol_columns(o.cwp, W, LW), "64 / 8: columns");
    o.shift = ol_read_shift(o.cwp, LW, 16);
    ol_read_table(&o, &p);
    ol_rows(&o, H);
    check(!o.c8 && o.ry[1] == H / LH && o.ry[LH - 1] == (LH - 1) * H / LH, "64 / 8: cells of 4 pairs, the rows of cells");
    for (y = 0; y < H; y++)
        for (x = 0; x < W / 2; x++)
            pic[y][x] = 128u << 8 | 128u << 24 | (x * 2 + y * 3) % 256 | (u32)((x * 2 + 1 + y * 3) % 256) << 16;
    memset(o.pb, 0, 2 * LW * LH);
    memset(o.ph, 0, 2 * LW * LH);
    memset(o.nrow, 0, LH);
    memset(o.y, 0, LW * LH);
    for (cy = 0; cy < LH - 1; cy++)         /* the last row of cells not read: from the layer's Y (all 0) */
        ol_read_cells(&o, pic[cy * 8 + 3], cy);
    ol_sources(&o, &p);
    for (cy = 0; cy < LH - 1; cy++)
        for (cx = 0; cx < LW; cx++) {
            for (x = cx * 8, e = 0; x < cx * 8 + 8; x++) {
                i = (x * 1 + (cy * 8 + 3) * 3) % 256;
                e += lin[i] > p.tb ? lin[i] - p.tb : 0;
            }
            want = e / 8;
            i = o.pb[cy * LW + cx];
            same &= i + 2 >= want && i <= want + 2;
        }
    check(same, "the read: the mean excess of each cell");
    for (cx = 0, same = 1; cx < LW; cx++)
        same &= o.pb[(LH - 1) * LW + cx] == 0 && o.ph[(LH - 1) * LW + cx] == 0;
    check(same, "a row of cells not read: from the layer (black - nothing)");
}

/* The sigma of the blur, measured: a line source (one column at 4095) blurred, the second moment of a row */
#define BW 160
#define BH 24

static double measured(u32 s1, u32 s2, u32 mix)
{
    ol_t o;
    u32 x, y;
    double m = 0, mx = 0, v = 0;

    layout(&o, BW, BH);
    memset(o.pb, 0, 2 * BW * BH);
    for (y = 0; y < BH; y++)
        o.pb[y * BW + BW / 2] = OL_ONE;
    ol_blur2(&o, o.pb, s1, s2, mix);
    y = BH / 2;
    for (x = 0; x < BW; x++) {
        m += o.pb[y * BW + x];
        mx += (double)x * o.pb[y * BW + x];
    }
    mx /= m;
    for (x = 0; x < BW; x++)
        v += (x - mx) * (x - mx) * o.pb[y * BW + x];
    return v / m;
}

static int near(double var, double want, double tol)
{
    return var > want * (1 - tol) * (1 - tol) && var < want * (1 + tol) * (1 + tol);
}

static void blurs(void)
{
    ol_t o;
    u32 i, sum1 = 0, r, a;
    int same = 1;
    char what[96];
    static const u32 sig[] = {256, 384, 512, 768, 1024, 1536, 2560, 4096};

    layout(&o, LW, LH);
    for (i = 0; i < LW * LH; i++)
        o.pb[i] = 1000;
    ol_gauss(&o, o.pb, 512);
    for (i = 0; i < LW * LH; i++)
        same &= o.pb[i] >= 999 && o.pb[i] <= 1001;
    check(same, "the Gaussian keeps a constant");
    for (i = 0; i < sizeof(sig) / sizeof(sig[0]); i++) {
        double s = sig[i] / 256.0, var = measured(sig[i], 0, 0);
        snprintf(what, sizeof(what), "sigma %.2f: measured %.3f (within 3 %%)", s, var > 0 ? ol_isqrt((u32)(var * 10000)) / 100.0 : 0);
        check(near(var, s * s, 0.03), what);
    }
    {
        double var = measured(512, 2048, 96), want = (160.0 * 4 + 96.0 * 64) / 256;
        snprintf(what, sizeof(what), "two Gaussians 2 and 8, weight 96/256: variance %.2f, want %.2f", var, want);
        check(near(var, want, 0.03), what);
    }
    ol_box_plan(0, &r, &a);
    check(r == 0 && a == 0, "sigma 0: a plain copy");
    layout(&o, LW, LH);
    memset(o.pb, 0, 2 * LW * LH);
    o.pb[3 * LW + 4] = 4000;
    ol_gauss(&o, o.pb, 256);
    for (i = 0; i < LW * LH; i++)
        sum1 += o.pb[i];
    check(sum1 + 200 > 4000 && sum1 < 4050, "the Gaussian keeps the mass (inside the edges)");
    check(o.pb[3 * LW + 4] < 1000 && o.pb[3 * LW + 5] > 0 && o.pb[2 * LW + 4] > 0, "the Gaussian spreads");
    check(ol_floor_sigma(5184, 160) == 70 && ol_floor_sigma(5184, 324) == 35 && ol_floor_sigma(5184, 648) == 17,
          "the transport's sigma: 70 px at 160, 35 at 324, 17 at 648");
    check(ol_cpu_sigma(30, 35, 5184, 324) == 0, "a sigma below the transport's: no blur of ours");
    i = ol_cpu_sigma(60, 35, 5184, 324);
    check(i > 3 * 256 - 20 && i < 3 * 256 + 20, "sigma 60 px over 35 at 324: about 3 cells");
    check(ol_isqrt(0) == 0 && ol_isqrt(1) == 1 && ol_isqrt(99) == 9 && ol_isqrt(100) == 10 && ol_isqrt(4000000) == 2000,
          "the integer square root");
}

static void words(void)
{
    ol_t o;
    ol_par_t p;
    u32 w, i;

    layout(&o, LW, LH);
    params(&p);
    memset(o.pb, 0, 2 * LW * LH);
    memset(o.ph, 0, 2 * LW * LH);
    ol_word_tables(&o, &p);
    o.y[0] = 40;
    w = ol_word(&o, &p, 0);
    check(w == ol_neutral(&p), "no excess: the neutral word");
    o.pb[0] = 400;
    w = ol_word(&o, &p, 0);
    check((w & 0xff) > 0 && (w >> 8 & 0xff) == 128 && (w >> 16 & 0xff) == 128, "Bloom: Y added, neutral");
    o.pb[0] = 0;
    o.ph[0] = 400;
    w = ol_word(&o, &p, 0);
    check((w & 0xff) > 0 && (w >> 8 & 0xff) < 128 && (w >> 16 & 0xff) > 128, "Halation: Y added, Cb down, Cr up");
    o.y[0] = 255;
    w = ol_word(&o, &p, 0);
    check((w & 0xff) == 0 && (w >> 16 & 0xff) == 128, "white: nothing to add");
    p.dark = 256;
    ol_word_tables(&o, &p);
    o.y[0] = 128;
    i = ol_word(&o, &p, 0) & 0xff;
    p.dark = 0;
    ol_word_tables(&o, &p);
    check(i < (ol_word(&o, &p, 0) & 0xff), "darkness: less Halation on a bright place");
    p.py = 2;
    p.pcb = 0;
    p.pcr = 1;
    check(ol_neutral(&p) == (128u | 128u << 8), "the neutral word by the positions");
    check(ol_pattern(&p, 0, 80) == (0x60u | 128u << 8), "pattern band 0: byte 0 = 0x60");
    check(ol_pattern(&p, 35, 80) == (128u | 128u << 8 | 0x60u << 24), "pattern band 3: byte 3 = 0x60");
    check(ol_pattern(&p, 79, 80) == ol_neutral(&p), "pattern band 7: neutral");
}

static void lanes(void)
{
    ol_lanes_t s;
    u32 i, py = 9, pcb = 9, pcr = 9;

    ol_lanes_init(&s);
    for (i = 0; i < 1000; i++)              /* byte 1 Y (spread), bytes 0 and 2 chroma near 128, byte 3 constant */
        ol_lanes_add(&s, (120 + i % 17) | ((i * 37) % 250) << 8 | (125 + i % 7) << 16 | 0u << 24);
    check(!ol_lane_order(&s, &py, &pcb, &pcr) && py == 1 && pcb == 0 && pcr == 2, "lane order: Y 1, Cb 0, Cr 2");
    ol_lanes_init(&s);
    for (i = 0; i < 1000; i++)
        ol_lanes_add(&s, 0x80808080u);
    check(ol_lane_order(&s, &py, &pcb, &pcr) == 1, "lane order: a flat layer is not enough");
    check(ol_bitrev4(1) == 8 && ol_bitrev4(2) == 4 && ol_bitrev4(3) == 12 && ol_bitrev4(15) == 15, "bit reversal");
}

/* ---- the fast code against the first version ---- */

#define MAXN (400 * 300)
static u16 refp[MAXN], refc[MAXN], reft[512];
static u32 wrow[MAXN];
static u8 rnrow[512];

/* A plane of a kind: 0 dense random, 1 sparse spots, 2 spots on the edges, 3 a few rows, 4 all 0, 5 constant, 6 random
 * with whole rows and columns of 0, 7 one value in a corner, 8 a few lights (small groups, as at night) */
static void fill(u16 *v, u32 lw, u32 lh, u32 kind)
{
    u32 i, n = lw * lh, x, y;

    memset(v, 0, 2 * n);
    switch (kind) {
    case 0:
        for (i = 0; i < n; i++)
            v[i] = (u16)rnd(OL_ONE + 1);
        break;
    case 1:
        for (i = 0; i < n / 50 + 1; i++)
            v[rnd(n)] = (u16)(1 + rnd(OL_ONE));
        break;
    case 2:
        for (i = 0; i < 6; i++) {
            v[rnd(lw)] = OL_ONE;                            /* the first row */
            v[(lh - 1) * lw + rnd(lw)] = (u16)rnd(OL_ONE + 1);  /* the last */
            v[rnd(lh) * lw] = OL_ONE;                       /* the first column */
            v[rnd(lh) * lw + lw - 1] = (u16)rnd(OL_ONE + 1);    /* the last */
        }
        break;
    case 3:
        for (y = 0; y < lh; y += 7)
            for (x = 0; x < lw; x++)
                v[y * lw + x] = (u16)rnd(OL_ONE + 1);
        break;
    case 4:
        break;
    case 5:
        for (i = 0; i < n; i++)
            v[i] = 1234;
        break;
    case 6:
        for (y = 0; y < lh; y++)
            for (x = 0; x < lw; x++)
                v[y * lw + x] = (y % 5 == 2 || x % 9 == 4) ? 0 : (u16)rnd(OL_ONE + 1);
        break;
    case 7:
        v[n - 1] = OL_ONE;
        break;
    default:
        for (i = 0; i < 6; i++) {
            x = rnd(lw);
            y = rnd(lh);
            v[y * lw + x] = OL_ONE;
            if (x + 1 < lw)
                v[y * lw + x + 1] = (u16)rnd(OL_ONE + 1);
            if (y + 1 < lh)
                v[(y + 1) * lw + x] = (u16)rnd(OL_ONE + 1);
        }
        break;
    }
}

/* The largest difference of a plane from the reference's */
static u32 diff(const u16 *a, const u16 *b, u32 n)
{
    u32 i, m = 0, d;

    for (i = 0; i < n; i++) {
        d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        if (d > m)
            m = d;
    }
    return m;
}

static void ref_over(ref_t *r, const ol_t *o)
{
    memset(r, 0, sizeof(*r));
    r->lw = o->lw;
    r->lh = o->lh;
    r->tmp = reft;
    r->pc = refc;
    r->lin = lin;
    r->enc = enc;
}

static u32 gauss_same(u32 lw, u32 lh, u32 kind, u32 s1, u32 s2, u32 mix, u32 *done, u32 *all)
{
    ol_t o;
    ref_t r;
    u32 n = lw * lh;

    layout(&o, lw, lh);
    ref_over(&r, &o);
    fill(o.pb, lw, lh, kind);
    memcpy(refp, o.pb, 2 * n);
    ol_blur2(&o, o.pb, s1, s2, mix);
    ref_blur2(&r, refp, s1, s2, mix);
    *done = o.done;
    *all = o.all;
    return diff(o.pb, refp, n);
}

static void gauss_equal(void)
{
    static const u32 sizes[][2] = {{324, 243}, {40, 30}, {41, 29}, {8, 6}, {3, 2}, {1, 7}, {7, 1}, {2, 2}, {250, 188}};
    static const u32 sig[] = {64, 100, 302, 500, 768, 1100, 1500, 2500, 4000, 8192, 9000};
    u32 si, gi, k, m, worst = 0, done, all, sparse_done = 0, sparse_all = 0, dense_ok = 1;
    char what[160];

    for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++)
        for (gi = 0; gi < sizeof(sig) / sizeof(sig[0]); gi++)
            for (k = 0; k < 9; k++) {
                if (sizes[si][0] * sizes[si][1] > 20000 && gi % 3 && k > 1 && k != 8)
                    continue;               /* the big layer: every third sigma on all kinds */
                m = gauss_same(sizes[si][0], sizes[si][1], k, sig[gi], 0, 0, &done, &all);
                if (m > worst)
                    worst = m;
                if (m) {
                    snprintf(what, sizeof(what), "Gaussian %ux%u, kind %u, sigma %u/256: differs from the first version by %u",
                             sizes[si][0], sizes[si][1], k, sig[gi], m);
                    check(0, what);
                }
                if (k == 8 && sizes[si][0] == 324 && sig[gi] >= 64) {
                    sparse_done += done;
                    sparse_all += all;
                }
                if (k == 0 && sig[gi] >= 64 && done != all)
                    dense_ok = 0;
            }
    m = gauss_same(324, 243, 1, 768, 302, 0, &done, &all);
    check(!m, "one Gaussian when the weight is 0: as the first version");
    for (k = 0; k < 9; k++) {
        m = gauss_same(324, 243, k, 512, 2048, 96, &done, &all);
        snprintf(what, sizeof(what), "two Gaussians (2 and 8 cells, weight 96), kind %u: differ by %u", k, m);
        check(!m, what);
        m = gauss_same(41, 29, k, 302, 1500, 200, &done, &all);
        snprintf(what, sizeof(what), "two Gaussians, an odd width, kind %u: differ by %u", k, m);
        check(!m, what);
    }
    check(dense_ok, "a dense plane: every value of every pass worked on");
    check(sparse_all && sparse_done < sparse_all / 2, "a few lights: the rows' ranges leave most of the work out");
    if (verbose)
        printf("Gaussians: the largest difference from the first version %u; a few lights: %u of %u values worked on\n", worst,
           sparse_done, sparse_all);
}

/* The pace callback: called after every pass; stops at the call asked */
static u32 pace_calls, pace_stop_at;

static u32 pace(void *ctx, u32 part)
{
    (void)ctx;
    (void)part;
    return ++pace_calls == pace_stop_at;
}

static void gauss_pace(void)
{
    ol_t o;

    layout(&o, 40, 30);
    fill(o.pb, 40, 30, 0);
    o.pace = pace;
    pace_calls = 0;
    pace_stop_at = 0;
    check(!ol_gauss(&o, o.pb, 768) && pace_calls == 2 * OL_BOX_PASSES, "pace: called after each of the 6 passes");
    pace_calls = 0;
    pace_stop_at = 3;
    check(ol_gauss(&o, o.pb, 768) == 1 && pace_calls == 3, "pace: a stop ends the Gaussian at once");
    pace_calls = 0;
    pace_stop_at = 2;
    check(ol_blur2(&o, o.pb, 512, 2048, 96) == 1 && pace_calls == 2, "pace: a stop ends the sum of two");
}

/* The read into the sums and the means against the first version, on a picture row of W pixels */
#define PW 5184
static u32 prow[16][PW / 2];

static u32 reads_same(u32 lw, u32 lh, u32 rows, u32 pack)
{
    ol_t o;
    ref_t r;
    ol_par_t p;
    u32 i, x, cy, n = lw * lh, m;

    layout(&o, lw, lh);
    ref_over(&r, &o);
    params(&p);
    if (ol_columns(o.cwp, PW, lw)) {
        check(0, "reads: columns");
        return 99;
    }
    o.shift = ol_read_shift(o.cwp, lw, 16);
    ol_read_table(&o, &p);
    ol_rows(&o, PW * 3 / 4);
    if (o.c8 != (lw == PW / 16)) {
        check(0, "reads: cells of 8 words exactly at 324");
        return 98;
    }
    o.c8 &= pack;                           /* 0: the generic read at 324 too */
    o.rbytes = rows & 1;
    r.cwp = o.cwp;
    r.shift = o.shift;
    r.pb = refp;
    r.ph = refc;
    r.y = o.y;
    r.nrow = rnrow;
    memset(o.pb, 0, 2 * n);
    memset(o.ph, 0, 2 * n);
    memset(refp, 0, 2 * n);
    memset(refc, 0, 2 * n);
    memset(o.nrow, 0, lh);
    memset(r.nrow, 0, lh);
    for (i = 0; i < n; i++)
        o.y[i] = (u8)rnd(256);
    for (i = 0; i < 16; i++)
        for (x = 0; x < PW / 2; x++)
            prow[i][x] = (rnd(3) ? rnd(256) : 190 + rnd(66)) | 128u << 8 | (u32)(rnd(3) ? rnd(256) : 190 + rnd(66)) << 16 |
                         128u << 24;
    for (cy = 0; cy < lh; cy++)             /* rows of cells read 0..rows times (0: from the layer's Y) */
        for (i = 0; i < (cy * 7 + 3) % (rows + 1); i++) {
            ol_read_cells(&o, prow[(cy + i) % 16], cy);
            ref_read_row(&r, &p, prow[(cy + i) % 16], cy);
        }
    m = diff(o.pb, refp, n);
    x = diff(o.ph, refc, n);
    if (x > m)
        m = x;
    if (m)
        return 100 + m;                     /* the sums must be the same */
    ol_sources(&o, &p);
    ref_sources(&r, &p);
    m = diff(o.pb, refp, n);
    x = diff(o.ph, refc, n);
    return x > m ? x : m;
}

static void reads_equal(void)
{
    char what[120];
    u32 m;

    m = reads_same(324, 243, 16, 1);
    snprintf(what, sizeof(what), "the read (packed) and the means at 324 (cells of 8 pairs): differ by %u", m);
    check(!m, what);
    m = reads_same(324, 243, 16, 0);
    snprintf(what, sizeof(what), "the read (generic) and the means at 324: differ by %u", m);
    check(!m, what);
    m = reads_same(324, 243, 3, 1);
    snprintf(what, sizeof(what), "the read and the means at 324, up to 3 rows a cell: differ by %u", m);
    check(!m, what);
    m = reads_same(400, 300, 16, 1);
    snprintf(what, sizeof(what), "the read and the means at 400: differ by %u", m);
    check(!m, what);
    m = reads_same(250, 188, 16, 1);           /* cells of 10..11 pairs: the read splits a cell's sum after 8 words */
    snprintf(what, sizeof(what), "the read and the means at 250: differ by %u", m);
    check(!m, what);
    m = reads_same(42, 32, 16, 1);             /* cells of 61..62 pairs; the means by division */
    snprintf(what, sizeof(what), "the read and the means at 42: differ by %u", m);
    check(!m, what);
    if (verbose)
        printf("reads: the same sums and means at 324, 400, 250 and 42\n");
}

/* The words against the first version: random blurs (many 0), Y and settings */
static void words_equal(void)
{
    ol_t o;
    ref_t r;
    ol_par_t p;
    u32 i, k, n = 324 * 243, bad = 0;
    volatile u32 *row = wrow;
    static const s32 hc[][2] = {{-102, 179}, {0, 0}, {256, -256}, {-40 * 256 / 100, 70 * 256 / 100}};

    layout(&o, 324, 243);
    ref_over(&r, &o);
    r.pb = o.pb;
    r.ph = o.ph;
    r.y = o.y;
    for (k = 0; k < 8; k++) {
        params(&p);
        p.gb = rnd(5 * 256);
        p.gh = rnd(5 * 256);
        p.dark = rnd(257);
        p.hcb = hc[k % 4][0];
        p.hcr = hc[k % 4][1];
        p.py = k % 3;
        p.pcb = (k + 1) % 3;
        p.pcr = (k + 2) % 3;
        for (i = 0; i < n; i++) {
            o.pb[i] = rnd(k < 4 ? 3 : 40) ? 0 : (u16)rnd(OL_ONE + 1);    /* then sparse: runs of neutral words */
            o.ph[i] = rnd(k < 4 ? 3 : 40) ? 0 : (u16)rnd(OL_ONE + 1);
            o.y[i] = (u8)rnd(256);
        }
        ol_word_tables(&o, &p);
        for (i = 0; i < n; i += 324) {
            if (k & 1) {                    /* pieces that start and end between words of the planes */
                ol_words(&o, &p, i, row + i, 161);
                ol_words(&o, &p, i + 161, row + i + 161, 1);
                ol_words(&o, &p, i + 162, row + i + 162, 162);
            } else {
                ol_words(&o, &p, i, row + i, 324);
            }
        }
        for (i = 0; i < n; i++)
            if (row[i] != ref_word(&r, &p, i) || ol_word(&o, &p, i) != row[i])
                bad++;
    }
    check(!bad, "the words: the same as the first version's");
    if (verbose)
        printf("words: %u of %u differ from the first version\n", bad, 8 * n);
}

int main(int argc, char **argv)
{
    (void)argv;
    verbose = argc > 1;
    curves();
    columns();
    reads();
    blurs();
    words();
    lanes();
    gauss_equal();
    gauss_pace();
    reads_equal();
    words_equal();
    if (!failed)
        printf("ok\n");
    return failed;
}

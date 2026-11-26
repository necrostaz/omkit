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

/* ---- the later parts: the emission, Bloom by darkness, Halation first, Bloom's tail ---- */

/* The emission table: 0 under the threshold, a blown pixel OL_ONE, steeper than the excess near 255, the review's numbers
 * at m 16 and the threshold 200 (245 ~0.35, 230 ~0.13 of a blown pixel); m 0 - the excess as it was */
static void emission(void)
{
    ol_t o;
    ol_par_t p;
    u32 c, mono = 1, e245, e230;

    layout(&o, LW, LH);
    params(&p);
    p.em = p.emb = 16;
    ol_read_table(&o, &p);
    for (c = 1; c < 256; c++)
        mono &= (o.tab[c] & 0xffff) >= (o.tab[c - 1] & 0xffff) && (o.tab[c] >> 16) >= (o.tab[c - 1] >> 16);
    check(mono, "emission: monotonic");
    check((o.tab[255] & 0xffff) == OL_ONE && (o.tab[255] >> 16) == OL_ONE, "emission: a blown pixel OL_ONE");
    check(!(o.tab[200] & 0xffff) && !(o.tab[230] >> 16) && (o.tab[201] & 0xffff), "emission: 0 at the threshold");
    e245 = o.tab[245] & 0xffff;
    e230 = o.tab[230] & 0xffff;
    if (verbose)
        printf("emission at m 16, threshold 200: 245 -> %u, 230 -> %u of %u\n", e245, e230, OL_ONE);
    check(e245 > OL_ONE * 30 / 100 && e245 < OL_ONE * 40 / 100, "emission: 245 ~0.35 of a blown pixel");
    check(e230 > OL_ONE * 10 / 100 && e230 < OL_ONE * 16 / 100, "emission: 230 ~0.13 of a blown pixel");
    check(e245 * (OL_ONE - p.tb) < (lin[245] - p.tb) * OL_ONE, "emission: below the excess (each to its blown pixel)");
    p.em = p.emb = 1;                       /* no shoulder: the excess, normalised */
    ol_read_table(&o, &p);
    check((o.tab[245] & 0xffff) + 1 >= (lin[245] - p.tb) * OL_ONE / (OL_ONE - p.tb) &&
          (o.tab[245] & 0xffff) <= (lin[245] - p.tb) * OL_ONE / (OL_ONE - p.tb) + 1, "emission at m 1: the excess normalised");
    p.em = p.emb = 0;
    ol_read_table(&o, &p);
    check((o.tab[245] & 0xffff) == lin[245] - p.tb && (o.tab[245] >> 16) == lin[245] - p.th, "m 0: the excess");
    p.em = p.emb = 16;
    p.th = OL_ONE;                          /* a threshold at white: nothing, no division by 0 */
    ol_read_table(&o, &p);
    check(!(o.tab[255] >> 16), "emission: a threshold at white gives nothing");
    p.th = lin[230];                        /* Bloom's own emission: its half alone changes */
    p.emb = 4;
    ol_read_table(&o, &p);
    e230 = o.tab[230] & 0xffff;
    e245 = o.tab[245] >> 16;
    p.emb = 16;
    ol_read_table(&o, &p);
    check((o.tab[230] & 0xffff) < e230 && (o.tab[245] >> 16) == e245,
          "emission: Bloom's own m (4 glows more from a bright surface than 16), Halation's half as it was");
}

/* The words with the later keys: Bloom less on a bright place (bdark); Halation first - its colour the same however
 * strong Bloom is, and the Y the same as Bloom first while nothing clips */
static void words_later(void)
{
    ol_t o;
    ol_par_t p;
    u32 a, b, w0, w1, w2;

    layout(&o, LW, LH);
    params(&p);
    ol_word_tables(&o, &p);
    o.y[0] = 200;
    a = ol_word_of(&o, &p, 400, 0, 200) & 0xff;
    p.bdark = 256;
    ol_word_tables(&o, &p);
    b = ol_word_of(&o, &p, 400, 0, 200) & 0xff;
    check(b < a && b > 0, "bloom_dark: less Bloom on a bright place");
    check((ol_word_of(&o, &p, 400, 0, 20) & 0xff) == (ol_word_of(&o, &p, 400, 0, 20) & 0xff), "bloom_dark: deterministic");
    p.bdark = 0;
    p.hfirst = 1;
    ol_word_tables(&o, &p);
    w0 = ol_word_of(&o, &p, 0, 300, 20);
    w1 = ol_word_of(&o, &p, 600, 300, 20);
    w2 = ol_word_of(&o, &p, 2000, 300, 20);
    check((w0 >> 16 & 0xff) == (w1 >> 16 & 0xff) && (w1 >> 16 & 0xff) == (w2 >> 16 & 0xff) && (w0 >> 16 & 0xff) > 128,
          "halation_first: the colour does not fade under Bloom");
    a = w1 & 0xff;
    p.hfirst = 0;
    ol_word_tables(&o, &p);
    check((ol_word_of(&o, &p, 600, 300, 20) & 0xff) == a, "halation_first: the same Y while nothing clips");
    check((ol_word_of(&o, &p, 2000, 300, 20) >> 16 & 0xff) < (w2 >> 16 & 0xff), "Bloom first: the colour fades");
}

/* The first tail's mix (8998668): pb = ((256 - w) pb + w T + 128) >> 8 - what ol_tail_add with Bloom's gain scaled by
 * (256 - w) / 256 stands for */
static void tail_mix_ref(ol_t *o, u32 w)
{
    u32 y, jy = 0, qy = 0, j, tn = (o->lw + OL_TAIL - 1) / OL_TAIL, iw = 256 - w, a, b, c, q, t, x;
    u32 v[128];
    const u16 *r0, *r1;
    u16 *row;

    for (y = 0; y < o->lh; y++) {
        row = o->pb + y * o->lw;
        r0 = o->pt + (qy < 2 ? (jy ? jy - 1 : 0) : jy) * o->tw;
        r1 = o->pt + (qy < 2 ? jy : (jy + 1 < o->th ? jy + 1 : jy)) * o->tw;
        for (j = 0; j < tn; j++)
            v[j] = qy == 0 ? 3 * r0[j] + 5 * r1[j] : qy == 1 ? r0[j] + 7 * r1[j] : qy == 2 ? 7 * r0[j] + r1[j] :
                   5 * r0[j] + 3 * r1[j];
        for (j = 0, a = v[0]; j < tn; j++, a = b) {
            b = v[j];
            c = j + 1 < tn ? v[j + 1] : b;
            for (q = 0; q < 4 && (x = 4 * j + q) < o->lw; q++) {
                t = q == 0 ? 3 * a + 5 * b : q == 1 ? a + 7 * b : q == 2 ? 7 * b + c : 5 * b + 3 * c;
                row[x] = (u16)((row[x] * iw + ((t + 32) >> 6) * w + 128) >> 8);
            }
        }
        if (++qy == OL_TAIL) {
            qy = 0;
            jy++;
        }
    }
}

/* Bloom's tail: the reduction and the enlargement keep the light's sum and a flat plane; added at w / (256 - w), the
 * light reaches farther than the main Gaussian's; its spread (second moment) is the tail's sigma (x OL_TAIL) and the
 * reduction's and enlargement's own; with Bloom's gain scaled by (256 - w) / 256 the light is the first version's mix
 * (within the rounding); a row the tail does not reach is left as it is */
static void tail(void)
{
    ol_t o;
    u32 i, x, y, n, lw = 324, lh = 243, far0, far1, w, k, dmax = 0, same;
    double s0, s1, m, v, d;
    static u16 keep[324 * 243], mixed[324 * 243];

    layout(&o, lw, lh);
    n = lw * lh;
    for (i = 0; i < n; i++)
        o.pb[i] = 1000;
    ol_tail_down(&o);
    for (i = 0, x = 1; i < o.tw * o.th; i++)
        x &= o.pt[i] == 1000;
    check(o.tw == 82 && o.th == 61 && x, "tail: an even plane (81 cells + a copy), a flat plane's reduction flat");
    ol_tail_add(&o, ol_tail_factor(128));
    for (i = 0, x = 1; i < n; i++)
        x &= o.pb[i] == 2000;
    check(x, "tail: a flat plane stays flat (G + T at w 1/2)");
    memset(o.pb, 0, 2 * n);                 /* a light of 3 x 3 in the middle */
    for (y = 120; y < 123; y++)
        for (x = 160; x < 163; x++)
            o.pb[y * lw + x] = 4000;
    ol_tail_down(&o);
    ol_gauss(&o, o.pb, 3 * 256);            /* the main Gaussian: 3 layer pixels */
    far0 = o.pb[121 * lw + 161 + 40];
    o.pace = 0;
    ol_tail_blur(&o, 5 * 256);              /* 5 tail pixels: 20 layer pixels */
    memcpy(keep, o.pb, 2 * n);
    memcpy(mixed, o.pt, 2 * o.tw * o.th);
    ol_tail_add(&o, ol_tail_factor(128));
    far1 = o.pb[121 * lw + 161 + 40];
    check(far0 == 0 && far1 > 0, "tail: the light reaches farther");
    for (w = 26; w <= 230; w += 51) {        /* against the first version's mix, Bloom's gain scaled */
        memcpy(o.pb, keep, 2 * n);
        memcpy(o.pt, mixed, 2 * o.tw * o.th);
        tail_mix_ref(&o, w);
        for (i = 0; i < n; i++)
            o.pc[i] = o.pb[i];
        memcpy(o.pb, keep, 2 * n);
        ol_tail_add(&o, ol_tail_factor(w));
        for (i = 0; i < n; i++) {
            k = (o.pb[i] * (256 - w) + 128) >> 8;
            d = (double)k - o.pc[i];
            if (d < 0)
                d = -d;
            if (d > dmax)
                dmax = (u32)d;
        }
        memcpy(o.pt, mixed, 2 * o.tw * o.th);   /* ol_tail_add scaled it */
    }
    if (verbose)
        printf("tail: against the first version's mix (w 10..90 %%): at most %u of %u apart\n", dmax, OL_ONE);
    check(dmax <= 2, "tail: the first version's light within the rounding of the gain's scaling");
    memset(o.pb, 0, 2 * n);                 /* a row the tail does not reach: as it is */
    o.pb[3] = 77;
    memset(o.pt, 0, 2 * o.tw * o.th);
    ol_tail_add(&o, 256);
    for (i = 0, same = o.pb[3] == 77; i < n; i++)
        same &= i == 3 || !o.pb[i];
    check(same, "tail: no tail - nothing changed");
    memset(o.pb, 0, 2 * n);                 /* the tail alone (G 0, f 1): its sum and spread */
    for (y = 120; y < 124; y++)
        for (x = 160; x < 164; x++)
            o.pb[y * lw + x] = 4000;
    for (i = 0, s0 = 0; i < n; i++)
        s0 += o.pb[i];
    ol_tail_down(&o);
    memset(o.pb, 0, 2 * n);
    ol_tail_blur(&o, 5 * 256);
    ol_tail_add(&o, 256);
    for (i = 0, s1 = 0, m = 0; i < n; i++) {
        s1 += o.pb[i];
        m += (double)o.pb[i] * (i % lw);
    }
    m /= s1;
    if (verbose)
        printf("tail: the light's sum %.0f of %.0f\n", s1, s0);
    check(s1 > s0 * 0.98 && s1 < s0 * 1.02, "tail: the light's sum kept");
    for (i = 0, v = 0; i < n; i++) {
        d = (i % lw) - m;
        v += o.pb[i] * d * d;
    }
    v /= s1;
    if (verbose)
        printf("tail: variance %.1f layer pixels^2 (want 400 of the tail's Gaussian + ~2.7 of the bilinear enlargement)\n",
               v);
    check(near(v, 402.7, 0.03), "tail: its sigma the one asked for (x OL_TAIL), plus the enlargement's");
    check(ol_tail_factor(0) == 0 && ol_tail_factor(128) == 256 && ol_tail_factor(230) == 2265, "tail: its factor");
}

/* The scene's darkness: the layer's mean Y and its share at a code, the day's weight and the gains it scales */
static void scene(void)
{
    ol_t o;
    u32 i, mean, share, d[4] = {80, 110, 50, 150};

    layout(&o, 324, 243);
    for (i = 0; i < 324 * 243; i++)
        o.y[i] = 50;
    ol_scene(&o, 200, &mean, &share);
    check(mean == 50 && share == 0, "scene: a dark flat layer");
    for (i = 0; i < 324 * 243; i++)
        o.y[i] = i % 324 < 162 ? 255 : 51;
    ol_scene(&o, 200, &mean, &share);
    check(mean == 153 && share == 500, "scene: half blown");
    check(ol_day(50, 0, d) == 0 && ol_day(153, 500, d) == 256, "day: night 0, day 256");
    check(ol_day(95, 0, d) == 128 && ol_day(0, 100, d) == 128 && ol_day(95, 120, d) == 179, "day: the ramps, the larger");
    check(ol_day_gain(1000, 0, 20) == 1000 && ol_day_gain(1000, 256, 20) == 199 && ol_day_gain(1000, 128, 50) == 750 &&
          ol_day_gain(1000, 256, 100) == 1000, "day: the gains");
}

/* Black and white (the colour tables in force, driver: hcb = hcr = 0): every word's chroma neutral, its Y the colour
 * version's - the halation's light kept, its colour gone; both orders, Bloom by darkness on, over codes and lights */
static void words_mono(void)
{
    ol_t o;
    ol_par_t p, q;
    u32 b, h, c, f, wc, wm, y_same = 1, neutral = 1, coloured = 0;

    layout(&o, LW, LH);
    params(&p);
    p.dark = 230;
    p.bdark = 200;
    p.gb = 600;
    p.gh = 1500;
    for (f = 0; f < 2; f++) {
        p.hfirst = f;
        q = p;
        q.hcb = q.hcr = 0;
        for (c = 0; c < 256; c += 5)
            for (h = 0; h < 4096; h += 97)
                for (b = 0; b < 4096; b += 391) {
                    ol_word_tables(&o, &p);
                    wc = ol_word_of(&o, &p, b, h, c);
                    ol_word_tables(&o, &q);
                    wm = ol_word_of(&o, &q, b, h, c);
                    y_same &= (wc & 0xff) == (wm & 0xff);
                    neutral &= (wm >> 8 & 0xffff) == 0x8080 && !(wm >> 24);
                    coloured |= (wc >> 8 & 0xffff) != 0x8080;
                }
    }
    check(coloured, "black and white: the colour version's halation has colour");
    check(y_same, "black and white: the halation's light (Y) the colour version's");
    check(neutral, "black and white: no colour in any word");
}

/* Bloom's haze: the tail's sources get haze lin[y] of each pixel (mean over the cell), at most OL_ONE; without a tail
 * or at haze 0 nothing; the hz table */
static void haze(void)
{
    ol_t o;
    ol_par_t p;
    u32 i, n, lw = 324, lh = 243, x, ok;

    layout(&o, lw, lh);
    n = lw * lh;
    params(&p);
    p.haze = 4096 * 4 / 100;
    ol_word_tables(&o, &p);
    check(!o.hazed, "haze: none without a tail");
    p.tmix = 128;
    ol_word_tables(&o, &p);
    check(o.hazed && o.hz[255] == (OL_ONE * (4096 * 4 / 100) >> 12) && o.hz[0] == 0, "haze: the table");
    memset(o.pb, 0, 2 * n);
    for (i = 0; i < n; i++)
        o.y[i] = 200;
    ol_tail_down(&o);
    for (i = 0, ok = 1; i < o.tw * o.th; i++)
        ok &= o.pt[i] == o.hz[200];
    check(ok, "haze: a flat Y gives a flat tail of its haze");
    for (i = 0; i < n; i++)                 /* sources and haze over OL_ONE: held at it */
        o.pb[i] = OL_ONE;
    p.haze = 4096;
    ol_word_tables(&o, &p);
    ol_tail_down(&o);
    for (i = 0, ok = 1; i < o.tw * o.th; i++)
        ok &= o.pt[i] == OL_ONE;
    check(ok, "haze: the tail's sources at most OL_ONE");
    for (i = 0; i < n; i++) {               /* a dark layer with a bright half: the haze where the light is */
        o.pb[i] = 0;
        o.y[i] = i % lw < 162 ? 220 : 10;
    }
    p.haze = 4096 * 4 / 100;
    ol_word_tables(&o, &p);
    ol_tail_down(&o);
    x = o.pt[30 * o.tw + 10];
    check(x == o.hz[220] && o.pt[30 * o.tw + 70] == o.hz[10] && o.pt[30 * o.tw + 70] < x, "haze: by the light of the cell");
    p.haze = 0;                             /* haze 0: the first tail's sources exactly */
    ol_word_tables(&o, &p);
    for (i = 0; i < n; i++)
        o.pb[i] = (u16)(i * 7 % 4096);
    ol_tail_down(&o);
    memcpy(o.pc, o.pt, 2 * o.tw * o.th);
    o.hazed = 0;
    ol_tail_down(&o);
    check(!memcmp(o.pc, o.pt, 2 * o.tw * o.th), "haze: 0 - the sources as without it");
}

/* ---- card169: the cover read and the strength's curve ---- */

/* A picture of CH rows of cells (16 rows each) of PW pixels: dark with lights of 1..12 px and thin lines (the glints of a
 * wire), the 4:2:2 words as the camera's */
#define CH 12
#define CPH (16 * CH)
static u32 cpic[CPH][PW / 2];
static u8 cyy[CPH][PW];

static void cover_picture(u32 seed0)
{
    u32 x, y, k, n, x0, y0, w, h, v;

    seed = seed0;
    for (y = 0; y < CPH; y++)
        for (x = 0; x < PW; x++)
            cyy[y][x] = (u8)(20 + (x / 40 + y / 7) % 30);
    for (k = 0; k < 600; k++) {             /* blobs and lines */
        w = k % 3 ? 1 + rnd(12) : 20 + rnd(200);
        h = k % 3 ? 1 + rnd(12) : 1 + rnd(2);
        x0 = rnd(PW - w);
        y0 = rnd(CPH - h);
        v = 200 + rnd(56);
        for (y = y0; y < y0 + h; y++)
            for (x = x0; x < x0 + w; x++)
                cyy[y][x] = (u8)v;
    }
    for (y = 0; y < CPH; y++)
        for (x = 0, n = 0; x < PW; x += 2, n++)
            cpic[y][n] = cyy[y][x] | 128u << 8 | (u32)cyy[y][x + 1] << 16 | 128u << 24;
}

/* The cover read of the picture into o (lw wide, CH rows of cells), up to `upto` rows read in all (0 - all); pack 0 - the
 * generic loop even at 324; the means by ol_sources */
static void cover_read(ol_t *o, ol_par_t *p, u32 lw, u32 pack, u32 bytes, u32 upto)
{
    u32 cy, k, n = 0;

    layout(o, lw, CH);
    params(p);
    p->em = 16;
    p->emb = 4;
    p->tb = lin[180];
    p->th = lin[225];
    ol_columns(o->cwp, PW, lw);
    o->shift = ol_read_shift(o->cwp, lw, OL_COVER / 2);
    ol_read_table(o, p);
    ol_rows(o, CPH);
    o->c8 &= pack;
    o->rbytes = bytes;
    memset(o->pb, 0, 2 * lw * CH);
    memset(o->ph, 0, 2 * lw * CH);
    memset(o->nrow, 0, CH);
    memset(o->y, 0, lw * CH);
    for (cy = 0; cy < CH; cy++)
        for (k = 0; k < OL_COVER; k++, n++)
            if (!upto || n < upto)
                ol_cover_cells(o, cpic[ol_cover_y(o, cy, k, CPH)], cy, k);
    ol_sources(o, p);
}

/* The cover read against its definition: the mean of the read's table over Y0 of the rows 4 and 12 of each cell (the
 * sums of a row >> shift, the mean by division, as ol_sources) - packed and generic, by bytes and words, at 324, 250
 * and 42; the same twice (no state carried); a stop leaves the rows of cells not read to the layer's Y */
static void cover(void)
{
    static u16 sb[324 * CH], sh[324 * CH];
    ol_t o;
    ol_par_t p;
    u32 lw, v, cy, cx, k, x, i, b, h, y, bad = 0, rows = 1, x0, rb, rh, d, pk, means = 1;
    static const u32 widths[] = {324, 250, 42};

    cover_picture(4242);
    for (v = 0; v < 3; v++)
        for (pk = 0; pk < 4; pk++) {
            lw = widths[v];
            if (lw != 324 && pk)
                continue;
            cover_read(&o, &p, lw, pk >> 1 ? 0 : 1, pk & 1, 0);
            for (cy = 0; cy < CH; cy++) {
                for (cx = 0, x0 = 0; cx < lw; x0 += o.cwp[cx], cx++) {
                    for (k = 0, rb = rh = 0; k < OL_COVER; k++) {
                        y = ol_cover_y(&o, cy, k, CPH);
                        if (lw == 324)
                            rows &= y == 16 * cy + (k ? 12 : 4);
                        for (x = x0, b = h = 0; x < x0 + o.cwp[cx]; x++) {
                            b += o.tab[cpic[y][x] & 0xff] & 0xffff;
                            h += o.tab[cpic[y][x] & 0xff] >> 16;
                        }
                        rb += b >> o.shift;
                        rh += h >> o.shift;
                    }
                    d = (u32)o.cwp[cx] * OL_COVER;
                    i = cy * lw + cx;
                    bad += o.pb[i] != (rb << o.shift) / d || o.ph[i] != (rh << o.shift) / d;
                }
            }
            if (lw == 324 && !(pk >> 1))
                for (cy = 0; cy < CH; cy++)
                    means &= o.nrow[cy] == (OL_COVER | OL_MEANS);
            if (!v && !pk) {
                memcpy(sb, o.pb, sizeof(sb));
                memcpy(sh, o.ph, sizeof(sh));
            } else if (!v) {
                bad += memcmp(sb, o.pb, sizeof(sb)) != 0 || memcmp(sh, o.ph, sizeof(sh)) != 0;
            }
        }
    check(rows, "cover: the rows 4 and 12 of a cell of 16");
    check(means, "cover: the packed read makes the means itself");
    check(!bad, "cover: the means of the samples, packed and generic, by bytes and words, at 324, 250, 42");
    cover_read(&o, &p, 324, 1, 1, 0);
    check(!memcmp(sb, o.pb, sizeof(sb)) && !memcmp(sh, o.ph, sizeof(sh)), "cover: the same sources again (deterministic)");
    cover_read(&o, &p, 324, 1, 1, 2 * 5 + 1);   /* a stop after 5 rows of cells and one row of the 6th */
    for (i = 0, bad = 0; i < 324 * CH; i++)
        bad += i < 324 * 5 ? o.pb[i] != sb[i] || o.ph[i] != sh[i] : o.pb[i] != (o.tab[0] & 0xffff) || o.ph[i] != 0;
    check(!bad, "cover: a stop - the rows of cells read as before, the rest from the layer's Y (here 0)");
    if (verbose)
        printf("cover: the means of the samples at 324, 250, 42; deterministic; a stop falls back on the layer's Y\n");
}

/* The cover read against the full read (all 16 rows of a cell, both pixels: the first version's sums and means) on
 * pictures of lights and thin lines, and against the one row of card168 (row 0 of each cell): the picture's sum of the
 * Halation's sources kept (within 10 %), the error a cell well below the one row's; no cell 9 times its full value (one
 * row of 16: up to 16) */
static void cover_vs_full(void)
{
    static u16 ch_[324 * CH], fh[324 * CH];
    ol_t o;
    ol_par_t p;
    u32 s, i, cy, j, worst = 0;
    double sf = 0, sc = 0, ec = 0, e1 = 0, d;

    for (s = 0; s < 4; s++) {
        cover_picture(1000 + s);
        cover_read(&o, &p, 324, 1, 1, 0);
        memcpy(ch_, o.ph, sizeof(ch_));     /* the cover's */
        memset(o.pb, 0, sizeof(fh));        /* the full read: 16 rows of both pixels */
        memset(o.ph, 0, sizeof(fh));
        memset(o.nrow, 0, CH);
        o.y0 = 0;
        o.shift = ol_read_shift(o.cwp, 324, 16);
        for (cy = 0; cy < CH; cy++)
            for (j = 0; j < 16; j++)
                ol_read_cells(&o, cpic[16 * cy + j], cy);
        ol_sources(&o, &p);
        memcpy(fh, o.ph, sizeof(fh));
        for (i = 0; i < 324 * CH; i++) {
            sf += fh[i];
            sc += ch_[i];
            d = (double)ch_[i] - fh[i];
            ec += d * d;
            if (fh[i] > 20 && ch_[i] > 9 * fh[i] + 9)
                worst++;
        }
        memset(o.pb, 0, sizeof(fh));        /* the one row of card168's read */
        memset(o.ph, 0, sizeof(fh));
        memset(o.nrow, 0, CH);
        for (cy = 0; cy < CH; cy++)
            ol_read_cells(&o, cpic[16 * cy], cy);
        ol_sources(&o, &p);
        for (i = 0; i < 324 * CH; i++) {
            d = (double)o.ph[i] - fh[i];
            e1 += d * d;
        }
    }
    check(sc > 0.9 * sf && sc < 1.1 * sf, "cover vs full: the picture's sum of the sources kept within 10 %");
    check(ec < 0.6 * e1, "cover vs full: a cell's error well below the one row's of card168");
    check(!worst, "cover vs full: no cell 9 times its full value");
    if (verbose)
        printf("cover vs full: sum %.0f of %.0f; squared error a picture: cover %.0f, one row %.0f\n", sc, sf, ec, e1);
}

/* The strength's curve: none - the light as it was; the toe takes a weak light away and keeps a strong one less the toe;
 * the ceiling holds every light under it and keeps a weak one; both monotonic. In the words: a weak halation fades, a
 * strong one stays; the ceiling holds the code added */
static void curve(void)
{
    static u16 t[OL_CURVE];
    ol_t o;
    ol_par_t p;
    u32 i, x, mono = 1, ok = 1, a, b;

    ol_curve(t, 0, 0);
    for (i = 0; i < OL_CURVE; i++)
        ok &= t[i] == 2 * i;
    check(ok, "curve: none - x itself");
    ol_curve(t, 205, 0);                    /* the toe 5 % */
    for (i = 1, ok = 1; i < OL_CURVE; i++) {
        x = 2 * i;
        mono &= t[i] >= t[i - 1];
        ok &= t[i] <= x && (x < 4096 || t[i] + 205u >= x);
    }
    check(mono && ok && t[20] < 2 * 20 / 4 && t[2000] + 210u >= 4000, "curve: the toe - weak away, strong less the toe");
    ol_curve(t, 0, 2048);                   /* at most half a white */
    for (i = 1, ok = 1, mono = 1; i < OL_CURVE; i++) {
        mono &= t[i] >= t[i - 1];
        ok &= t[i] < 2048;
    }
    check(mono && ok && t[50] >= 95 && t[OL_CURVE - 1] > 1600, "curve: the ceiling - under it, a weak light kept");
    check(ol_curved(t, 1u << 20) == t[OL_CURVE - 1] && ol_curved(t, 9) == t[4], "curve: its entries");
    layout(&o, LW, LH);
    params(&p);
    p.hfirst = 1;
    ol_word_tables(&o, &p);
    check(!o.bc && !o.hc, "curve: off without a toe or a ceiling");
    a = ol_word_of(&o, &p, 0, 60, 20) & 0xff;
    b = ol_word_of(&o, &p, 0, 3000, 20) & 0xff;
    p.htoe = 410;                           /* 10 % */
    ol_word_tables(&o, &p);
    check(o.hc && (ol_word_of(&o, &p, 0, 60, 20) & 0xff) < a / 2 + 1 && (ol_word_of(&o, &p, 0, 3000, 20) & 0xff) + 12 >= b,
          "curve: the toe in the word - a weak halation fades, a strong one stays");
    p.htoe = 0;
    p.hmax = 410;
    ol_word_tables(&o, &p);
    check((ol_word_of(&o, &p, 0, 4000, 20) & 0xff) <= (u32)(o.enc[o.lin[20] + 410] - 20), "curve: the ceiling in the word");
    if (verbose)
        printf("curve: none, toe, ceiling; in the words\n");
}

/* ---- card170: the look's white, the shelf, the halation's colour and core, Bloom's colour ---- */

/* ol_white: W 255 - ol_luts's tables exactly (in place too); W 227 - white at W and past it, a pixel never past W, the
 * same sources and the same add for the same picture on its own scale */
static void white(void)
{
    static u16 l2[256];
    static u8 e2[OL_ENC];
    static u32 t0[256], y0[256];
    ol_t o, q;
    ol_par_t p;
    u32 c, v, ok = 1, mono = 1, past = 1, same = 1, srcd = 0, addd = 0, WL = 227, cw, a, b, n = 0;

    memcpy(l2, lin, sizeof(l2));
    ol_white(l2, e2, l2, 255);
    check(!memcmp(l2, lin, sizeof(l2)) && !memcmp(e2, enc, sizeof(e2)), "white: 255 - the curves as ol_luts made them");
    ol_white(l2, e2, lin, 100);             /* below OL_WHITE_MIN: taken as 255 */
    check(!memcmp(l2, lin, sizeof(l2)) && !memcmp(e2, enc, sizeof(e2)), "white: out of range - 255");
    memcpy(l2, lin, sizeof(l2));
    ol_white(l2, e2, l2, WL);                /* in place, as the worker does it */
    for (c = 1; c < 256; c++) {
        mono &= l2[c] >= l2[c - 1];
        ok &= c < WL || l2[c] == OL_ONE;
    }
    for (v = 0; v < OL_ENC; v++)
        past &= e2[v] <= WL && (v == 0 || e2[v] >= e2[v - 1]);
    for (c = 0; c <= WL; c++)
        same &= l2[c] == OL_ONE ? 1 : e2[l2[c]] == c || l2[c] == l2[c + 1];
    check(mono && ok && past && same && e2[OL_ONE] == WL, "white: 227 - white from WL on, the codes back, never past WL");
    /* the same scene on two looks: codes c on a look of white 255, round(c WL / 255) on the look of white WL - the sources
     * and the added code (scaled back) match */
    layout(&o, LW, LH);                     /* (one work buffer: the plain look's values kept first) */
    params(&p);
    p.tb = lin[180];                        /* the thresholds on the look's scale: from the curves before ol_white */
    p.th = lin[235];
    p.em = 16;
    p.emb = 4;
    p.hfirst = 1;
    ol_read_table(&o, &p);
    ol_word_tables(&o, &p);
    for (c = 0; c < 256; c++) {
        t0[c] = o.tab[c];
        y0[c] = ol_word_of(&o, &p, 600, 0, c) & 0xff;
    }
    layout(&q, LW, LH);
    ol_white((u16 *)q.lin, (u8 *)q.enc, q.lin, WL);
    ol_read_table(&q, &p);
    ol_word_tables(&q, &p);
    for (c = 0; c < 256; c++) {
        cw = (c * WL + 127) / 255;
        a = t0[c];
        b = q.tab[cw];
        v = (a & 0xffff) > (b & 0xffff) ? (a & 0xffff) - (b & 0xffff) : (b & 0xffff) - (a & 0xffff);
        srcd = v > srcd ? v : srcd;
        if (c < 250) {                      /* the add of one light, scaled back */
            a = y0[c];
            b = ol_word_of(&q, &p, 600, 0, cw) & 0xff;
            v = a * WL / 255 > b ? a * WL / 255 - b : b - a * WL / 255;
            addd = v > addd ? v : addd;
            n++;
        }
    }
    check(srcd < OL_ONE / 20 && addd <= 3, "white: the same scene on its own scale - the same sources and add");
    check((ol_word_of(&q, &p, 4000, 4000, WL) & 0xff) == 0 && (ol_word_of(&q, &p, 4000, 4000, 250) & 0xff) == 0,
          "white: nothing added at the look's white or past it (a sharpening's overshoot)");
    check(q.tab[250] == q.tab[WL], "white: an overshoot past WL is a source as white");
    if (verbose)
        printf("white: sources differ by at most %u (Q12), the add by %u of %u codes\n", srcd, addd, n);
}

/* ol_shelf: a blown part at the look's white is found; a smooth tail of the highlights or nothing blown - none */
static void shelf(void)
{
    static u32 hist[256];
    ol_t o;
    u32 i, n = 324 * 243;

    layout(&o, 324, 243);
    for (i = 0; i < n; i++)                 /* a room: codes 20..200, 2 % at 227 (a window), overshoots up to 255 */
        o.y[i] = (u8)(i % 50 == 0 ? 227 : i % 997 == 0 ? 240 + rnd(16) : 20 + rnd(181));
    check(ol_shelf(&o, hist, OL_WHITE_MIN) == 227, "shelf: the blown part's code");
    for (i = 0; i < n; i++)                 /* highlights falling off smoothly to 230: no shelf */
        o.y[i] = (u8)(i % 3 ? 20 + rnd(150) : 170 + (u32)(60.0 * (1.0 - (double)rnd(1000) * rnd(1000) / 1e6)));
    check(ol_shelf(&o, hist, OL_WHITE_MIN) == 0, "shelf: a smooth tail is none");
    for (i = 0; i < n; i++)                 /* a plateau below lo: none */
        o.y[i] = (u8)(i % 20 == 0 ? 150 : 20 + rnd(100));
    check(ol_shelf(&o, hist, OL_WHITE_MIN) == 0, "shelf: none below the lowest white");
    if (verbose)
        printf("shelf: found, a smooth tail none, below lo none\n");
}

/* The halation's colour from its edge's to its strong one's, and its white core */
static void halation_colour(void)
{
    ol_t o;
    ol_par_t p;
    s32 cb, cr;
    u32 w, a, b;

    layout(&o, LW, LH);
    params(&p);
    p.hcb = -75 * 256 / 100;                /* strong: orange */
    p.hcr = 120 * 256 / 100;
    p.hcb2 = -40 * 256 / 100;               /* edge: red */
    p.hcr2 = 120 * 256 / 100;
    p.hcode = 40;
    ol_word_tables(&o, &p);
    w = o.cw[10];
    cb = (s32)(w >> 8 & 0xff) - 128;
    cr = (s32)(w >> 16 & 0xff) - 128;
    check(cr >= 11 && cr <= 12 && cb >= -5 && cb <= -4, "halation colour: near the edge's at a weak code");
    w = o.cw[80];
    cb = (s32)(w >> 8 & 0xff) - 128;
    cr = (s32)(w >> 16 & 0xff) - 128;
    check(cr >= 95 && cr <= 96 && cb == -60, "halation colour: the strong one past its code");
    p.hcode = 0;
    ol_word_tables(&o, &p);
    check((o.cw[80] >> 8 & 0xff) == 128 - 60, "halation colour: one colour without a code");
    /* the core: a bright place gets less colour with hcore */
    p.dark = 256;
    p.hfirst = 1;
    ol_word_tables(&o, &p);
    a = ol_word_of(&o, &p, 0, 3000, 220) >> 16 & 0xff;
    p.hcore = 1;
    ol_word_tables(&o, &p);
    b = ol_word_of(&o, &p, 0, 3000, 220) >> 16 & 0xff;
    check(b < a && b >= 128 && (ol_word_of(&o, &p, 0, 3000, 220) & 0xff) == (ol_word_of(&o, &p, 0, 3000, 220) & 0xff),
          "halation core: less colour on a bright place, the same light");
    check((ol_word_of(&o, &p, 0, 3000, 20) >> 16 & 0xff) > 128 + 20, "halation core: the colour on the dark");
    if (verbose)
        printf("halation colour: edge cw[10] %08x, strong cw[80] %08x; core Cr %u -> %u\n", o.cw[10], o.cw[80], a, b);
}

/* Bloom's colour: a warm source's chromaticity on its glow, in proportion to Bloom's code; none without a source; the
 * enlargement smooth */
static void bloom_colour(void)
{
    ol_t o;
    ol_par_t p;
    u32 i, x, y, lw = 324, lh = 243, n = lw * lh, ok = 1, step = 0, w, d;
    s32 cb, cr, cb2, f0;

    layout(&o, lw, lh);
    params(&p);
    p.bcol = 256;                           /* 100 % */
    p.hfirst = 1;
    ol_word_tables(&o, &p);
    check(o.bq, "bloom colour: on");
    for (i = 0; i < n; i++) {
        x = i % lw;
        y = i / lw;
        o.y[i] = 30;
        o.pc[i] = 128 | 128 << 8;
        o.pb[i] = 0;
        if (x >= 100 && x < 120 && y >= 100 && y < 120) {   /* a warm lamp: Y 200, Cb 100, Cr 160 */
            o.y[i] = 200;
            o.pc[i] = 100 | 160 << 8;
            o.pb[i] = 2000;
        }
    }
    ol_colour_down(&o);
    check(!ol_colour_blur(&o, 3 * 256), "bloom colour: blurred");
    ol_colour_ratio(&o, p.bcol);
    ol_colour_up(&o);
    i = 110 * lw + 110;                     /* at the lamp: (Cb - 128) / Y = -0.14, Cr +0.16 in Q7 */
    f0 = (s32)(o.pc[i] & 0xff) - 128;
    check(f0 >= -19 && f0 <= -15 && (s32)(o.pc[i] >> 8) - 128 >= 17 && (s32)(o.pc[i] >> 8) - 128 <= 21,
          "bloom colour: the lamp's chromaticity");
    for (y = 60; y < 180; y++)              /* smooth: neighbours differ by a few Q7 steps at most (the fade where the
                                               sources' light ends is a tail pixel or two: little glow there) */
        for (x = 61; x < 180; x++) {
            d = (o.pc[y * lw + x] & 0xff) > (o.pc[y * lw + x - 1] & 0xff) ? (o.pc[y * lw + x] & 0xff) - (o.pc[y * lw + x - 1] & 0xff) :
                (o.pc[y * lw + x - 1] & 0xff) - (o.pc[y * lw + x] & 0xff);
            step = d > step ? d : step;
        }
    check(step <= 4, "bloom colour: the enlargement smooth");
    check(o.pc[5 * lw + 300] == (128 | 128 << 8), "bloom colour: neutral far from any source");
    w = ol_word_q(&o, &p, 1500, 0, 30, o.pc[i]);
    cb = (s32)(w >> 8 & 0xff) - 128;
    cr = (s32)(w >> 16 & 0xff) - 128;
    cb2 = (s32)(w & 0xff) * f0 / 128;
    ok = cb <= cb2 + 1 && cb >= cb2 - 1 && cr > 0 && (w & 0xff) > 20;
    check(ok, "bloom colour: chroma a unit of Bloom's code at the source's chromaticity");
    check((ol_word_q(&o, &p, 0, 1500, 30, o.pc[i]) >> 8 & 0xff) == (ol_word_of(&o, &p, 0, 1500, 30) >> 8 & 0xff),
          "bloom colour: none without Bloom's light");
    p.bcol = 0;
    ol_word_tables(&o, &p);
    check(!o.bq && ol_word(&o, &p, i) == ol_word_of(&o, &p, o.pb[i], o.ph[i], o.y[i]), "bloom colour: off - neutral");
    if (verbose)
        printf("bloom colour: Q7 %d at the lamp, largest step %u, word %08x\n", f0, step, w);
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
    emission();
    words_later();
    tail();
    scene();
    words_mono();
    haze();
    cover();
    cover_vs_full();
    curve();
    white();
    shelf();
    halation_colour();
    bloom_colour();
    if (!failed)
        printf("ok\n");
    return failed;
}

/* Host test of the optics layer's plain parts (src/platform/om5m2/fw1100/optlayer.h, the code the worker task runs): the
 * curves, the columns of cells, the read of the picture's rows into the sources (against a direct computation), the blur
 * (a constant stays, the mass stays, it spreads, its sigma - also of two Gaussians - the one asked for), the words (nothing added under the thresholds, light added and coloured
 * above them, no change of a white place), the pattern and the guess of the byte positions. Prints "ok", or what failed. */
#include <stdio.h>
#include <string.h>
#include "omk_types.h"
#include "optlayer.h"

static int failed;

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "%s\n", what);
        failed = 1;
    }
}

static u16 lin[256];
static u8 enc[OL_ENC];

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
static u8 ly[LW * LH], nrow[LH], cwp[LW];
static u16 pb[LW * LH], ph[LW * LH], pc[LW * LH], tmp[64];

static void setup(ol_t *o, ol_par_t *p)
{
    memset(o, 0, sizeof(*o));
    o->lw = LW;
    o->lh = LH;
    o->y = ly;
    o->pb = pb;
    o->ph = ph;
    o->pc = pc;
    o->tmp = tmp;
    o->nrow = nrow;
    o->cwp = cwp;
    o->lin = lin;
    o->enc = enc;
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

    setup(&o, &p);
    check(!ol_columns(cwp, W, LW), "64 / 8: columns");
    o.shift = ol_read_shift(cwp, LW, 16);
    for (y = 0; y < H; y++)
        for (x = 0; x < W / 2; x++)
            pic[y][x] = 128u << 8 | 128u << 24 | (x * 2 + y * 3) % 256 | (u32)((x * 2 + 1 + y * 3) % 256) << 16;
    memset(pb, 0, sizeof(pb));
    memset(ph, 0, sizeof(ph));
    memset(nrow, 0, sizeof(nrow));
    memset(ly, 0, sizeof(ly));
    for (cy = 0; cy < LH - 1; cy++)         /* the last row of cells not read: from the layer's Y (all 0) */
        ol_read_row(&o, &p, pic[cy * 8 + 3], cy);
    ol_sources(&o, &p);
    for (cy = 0; cy < LH - 1; cy++)
        for (cx = 0; cx < LW; cx++) {
            for (x = cx * 8, e = 0; x < cx * 8 + 8; x++) {
                i = (x * 1 + (cy * 8 + 3) * 3) % 256;
                e += lin[i] > p.tb ? lin[i] - p.tb : 0;
            }
            want = e / 8;
            i = pb[cy * LW + cx];
            same &= i + 2 >= want && i <= want + 2;
        }
    check(same, "the read: the mean excess of each cell");
    for (cx = 0, same = 1; cx < LW; cx++)
        same &= pb[(LH - 1) * LW + cx] == 0 && ph[(LH - 1) * LW + cx] == 0;
    check(same, "a row of cells not read: from the layer (black - nothing)");
}

/* The sigma of the blur, measured: a line source (one column at 4095) blurred, the second moment of a row */
#define BW 160
#define BH 24
static u16 big[BW * BH], big2[BW * BH], btmp[BW + 2];

static double measured(u32 s1, u32 s2, u32 mix)
{
    ol_t o;
    u32 x, y;
    double m = 0, mx = 0, v = 0;

    memset(&o, 0, sizeof(o));
    o.lw = BW;
    o.lh = BH;
    o.tmp = btmp;
    o.pc = big2;
    memset(big, 0, sizeof(big));
    for (y = 0; y < BH; y++)
        big[y * BW + BW / 2] = OL_ONE;
    ol_blur2(&o, big, s1, s2, mix);
    y = BH / 2;
    for (x = 0; x < BW; x++) {
        m += big[y * BW + x];
        mx += (double)x * big[y * BW + x];
    }
    mx /= m;
    for (x = 0; x < BW; x++)
        v += (x - mx) * (x - mx) * big[y * BW + x];
    return v / m;
}

static int near(double var, double want, double tol)
{
    return var > want * (1 - tol) * (1 - tol) && var < want * (1 + tol) * (1 + tol);
}

static void blurs(void)
{
    ol_t o;
    ol_par_t p;
    u32 i, sum1 = 0, r, a;
    int same = 1;
    char what[96];
    static const u32 sig[] = {256, 384, 512, 768, 1024, 1536, 2560, 4096};

    setup(&o, &p);
    o.pc = big2;
    for (i = 0; i < LW * LH; i++)
        pb[i] = 1000;
    ol_gauss(&o, pb, 512);
    for (i = 0; i < LW * LH; i++)
        same &= pb[i] >= 999 && pb[i] <= 1001;
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
    memset(pb, 0, sizeof(pb));
    pb[3 * LW + 4] = 4000;
    ol_gauss(&o, pb, 256);
    for (i = 0; i < LW * LH; i++)
        sum1 += pb[i];
    check(sum1 + 200 > 4000 && sum1 < 4050, "the Gaussian keeps the mass (inside the edges)");
    check(pb[3 * LW + 4] < 1000 && pb[3 * LW + 5] > 0 && pb[2 * LW + 4] > 0, "the Gaussian spreads");
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

    setup(&o, &p);
    memset(pb, 0, sizeof(pb));
    memset(ph, 0, sizeof(ph));
    ly[0] = 40;
    w = ol_word(&o, &p, 0);
    check(w == ol_neutral(&p), "no excess: the neutral word");
    pb[0] = 400;
    w = ol_word(&o, &p, 0);
    check((w & 0xff) > 0 && (w >> 8 & 0xff) == 128 && (w >> 16 & 0xff) == 128, "Bloom: Y added, neutral");
    pb[0] = 0;
    ph[0] = 400;
    w = ol_word(&o, &p, 0);
    check((w & 0xff) > 0 && (w >> 8 & 0xff) < 128 && (w >> 16 & 0xff) > 128, "Halation: Y added, Cb down, Cr up");
    ly[0] = 255;
    w = ol_word(&o, &p, 0);
    check((w & 0xff) == 0 && (w >> 16 & 0xff) == 128, "white: nothing to add");
    p.dark = 256;
    ly[0] = 128;
    i = ol_word(&o, &p, 0) & 0xff;
    p.dark = 0;
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

int main(void)
{
    curves();
    columns();
    reads();
    blurs();
    words();
    lanes();
    if (!failed)
        printf("ok\n");
    return failed;
}

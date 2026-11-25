/* FILMLOOK: the user's edits of a preset applied to its ISPB block (edits.h). The block (OMKit/docs/formats/isp.md,
 * section 5): +2 u16 the parts present (bit 0 the matrix), +8 s16[9] the matrix F (Q7, rows sum to 128), +28 u16 Cb gain,
 * +32 u16[3][256] the gamma R, G, B (input node i = i/255 of the full scale, output 0..4095), +1568 u16[256] the tone
 * curve on Y (node i at Y = i/256, output 0..1023; past node 255 the last segment continues), +2080 u16 the enabled
 * sectors, +2088 12 sector records of 24 bytes (+0 h0 and +2 hw of the hue window, 32768 a turn; +20 s16 the saturation,
 * 1/1023 a unit). Color/Vivid shifts a sector's saturation from the preset's, as the camera's own editor starts every
 * hue at 0: the edited one is the preset's + the shift x SAT_STEP, kept within +-SAT_MAX */
#include "edits.h"
#include "omk_ispb.h"

#define OFF_PRESENT OMK_ISPB_PRESENT     /* the block's layout: omk_ispb.h */
#define OFF_MATRIX OMK_ISPB_MATRIX
#define OFF_GAMMA_G (OMK_ISPB_GAMMA + 512)
#define OFF_TONE OMK_ISPB_TONE
#define OFF_MASK OMK_ISPB_SECTOR_MASK
#define OFF_REC OMK_ISPB_SECTORS
#define REC 24
#define REC_SAT 20
#define SAT_STEP 51                         /* 0.05 of saturation in the record's units (1/1023) */
#define SAT_MAX 1023                        /* +-1.0: the edited saturation stays within it */
#define TONE_STEP 20                        /* a tone step: 20 of 1023 of the output, about 2 % */
#define TURN 32768

/* The inputs where the three points and the 64 points of the drawn curve sit: u = 1/4, 1/2, 3/4 and k/63 of a
 * perceptual axis, linear L = u^2.2, as a position on the gamma's input in 1/16 of a node (16 x 255 x L, rounded) */
static const u16 point_at[3] = {193, 888, 2167};
static const u16 curve_at[64] = {
    0, 0, 2, 5, 9, 15, 23, 32, 44, 56, 71, 88, 106, 127, 149, 174, 200, 229, 259, 292, 327, 364, 403, 445, 488, 534, 582,
    633, 685, 740, 798, 857, 919, 984, 1050, 1120, 1191, 1265, 1342, 1421, 1502, 1586, 1672, 1761, 1852, 1946, 2043, 2142,
    2243, 2347, 2454, 2563, 2675, 2789, 2907, 3026, 3149, 3274, 3401, 3532, 3665, 3800, 3939, 4079};
/* sin(2 pi k / 64), Q12 */
static const s16 sin64[64] = {
    0, 401, 799, 1189, 1567, 1931, 2276, 2598, 2896, 3166, 3406, 3612, 3784, 3920, 4017, 4076, 4096, 4076, 4017, 3920, 3784,
    3612, 3406, 3166, 2896, 2598, 2276, 1931, 1567, 1189, 799, 401, 0, -401, -799, -1189, -1567, -1931, -2276, -2598, -2896,
    -3166, -3406, -3612, -3784, -3920, -4017, -4076, -4096, -4076, -4017, -3920, -3784, -3612, -3406, -3166, -2896, -2598,
    -2276, -1931, -1567, -1189, -799, -401};
/* The camera's own black-and-white filter matrices (Q8, rows of 256): filter 1..8 = YELLOW, ORANGE, RED, GREEN,
 * GREEN_YELLOW, MAGENTA, BLUE, CYAN, level 1..3 - read from the firmware: the lookup table 0x605756d0 {0x80 | level << 4 |
 * filter, handler}, each handler mov matrix,a0 (matrices at 0x605522e2..0x605524c8). GREEN_YELLOW level 1 is so in the
 * firmware: its rows do not sum to 256 (the diagonal is brought back below, as the firmware's 0x603e4f0c does) */
static const s16 filters[8][3][9] = {
    {{275, -27, 8, 9, 265, -18, 15, 29, 212}, {294, -54, 16, 18, 274, -36, 30, 58, 168}, {313, -81, 24, 27, 283, -54, 45, 87, 124}},
    {{285, -25, -4, 21, 252, -17, 81, -52, 227}, {314, -50, -8, 42, 248, -34, 162, -104, 198},
     {343, -75, -12, 63, 244, -51, 243, -156, 169}},
    {{332, -115, 39, 97, 200, -41, 116, -36, 176}, {408, -230, 78, 194, 144, -82, 232, -72, 96},
     {484, -345, 117, 291, 88, -123, 348, -108, 16}},
    {{195, 31, 30, -5, 280, -19, -101, 168, 189}, {134, 62, 60, -10, 304, -38, -202, 336, 122},
     {73, 93, 90, -15, 328, -57, -303, 504, 55}},
    {{235, -145, 19, 2, 254, -19, -43, 43, 200}, {214, 4, 38, 4, 290, -38, -86, 198, 144}, {193, 6, 57, 6, 307, -57, -129, 297, 88}},
    {{267, -123, 112, 67, 161, 28, 157, -154, 253}, {278, -246, 224, 134, 66, 56, 314, -308, 250},
     {289, -369, 336, 201, -29, 84, 471, -462, 247}},
    {{176, -36, 116, -41, 200, 97, 39, -115, 332}, {96, -72, 232, -82, 144, 194, 78, -230, 408},
     {16, -108, 348, -123, 88, 291, 117, -345, 484}},
    {{134, -17, 139, -51, 228, 79, -68, 61, 263}, {12, -34, 278, -102, 200, 158, -136, 122, 270},
     {-110, -51, 417, -153, 172, 237, -204, 183, 277}},
};

static u32 rd16(const u8 *p)
{
    return p[0] | p[1] << 8;
}

static void wr16(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

/* A 256-node table at p (1/16 of a node), linear between the nodes; past node 255 its last segment continues. The tables
 * are not decreasing, so every term is not negative */
static u32 at16(const u8 *tab, u32 p)
{
    u32 i = p >> 4, f = p & 15, a, b;

    if (i >= 255) {
        a = rd16(tab + 2 * 254);
        b = rd16(tab + 2 * 255);
        return b + (b > a ? (b - a) * ((i - 255) * 16 + f) / 16 : 0);
    }
    a = rd16(tab + 2 * i);
    b = rd16(tab + 2 * i + 2);
    return (a * (16 - f) + b * f) / 16;
}

/* The tone curve through the three points moved by v[k] steps: the change is 0 at both ends and v[k] x TONE_STEP at the
 * tone input of point k (where the gamma takes it), smoothstep between; the result clipped to 0..1023, never falling */
static void tone_edit(u8 *ispb, const signed char *v)
{
    const u8 *g = ispb + OFF_GAMMA_G;
    u8 *t = ispb + OFF_TONE;
    u32 px[5], n = 1, k, j, a, x, q, s;
    s32 dy[5], d, prev = 0;

    if (!v[0] && !v[1] && !v[2])
        return;
    px[0] = 0;
    dy[0] = 0;
    for (k = 0; k < 3; k++) {
        a = at16(g, point_at[k]);           /* 0..4095: the tone input, 16 to a node */
        if (a > px[n - 1] && a < 4096) {
            px[n] = a;
            dy[n] = v[k] * TONE_STEP;
            n++;
        }
    }
    px[n] = 4096;
    dy[n] = 0;
    n++;
    for (j = 0, k = 0; j < 256; j++) {
        x = j * 16;
        while (k + 2 < n && x >= px[k + 1])
            k++;
        q = (x - px[k]) * 1024 / (px[k + 1] - px[k]);
        s = q * q * (3072 - 2 * q) >> 20;   /* 3q^2 - 2q^3, 0..1024 */
        d = dy[k] + (((dy[k + 1] - dy[k]) * (s32)s) >> 10);
        d += (s32)rd16(t + 2 * j);
        if (d < 0)
            d = 0;
        if (d > 1023)
            d = 1023;
        if (d < prev)
            d = prev;
        prev = d;
        wr16(t + 2 * j, (u32)d);
    }
}

void edits_apply(u8 *ispb, const edits_t *e)
{
    u32 i, r, c;

    tone_edit(ispb, e->tone);
    for (i = 0; i < 12; i++)
        if (e->vivid[i]) {
            s32 a = (s16)rd16(ispb + OFF_REC + REC * i + REC_SAT) + e->vivid[i] * SAT_STEP;

            wr16(ispb + OFF_REC + REC * i + REC_SAT, (u32)(u16)(s16)(a < -SAT_MAX ? -SAT_MAX : a > SAT_MAX ? SAT_MAX : a));
        }
    if (e->filter >= 1 && e->filter <= 8 && e->level >= 1 && e->level <= 3) {
        const s16 *m = filters[e->filter - 1][e->level - 1];
        s32 out[9], sum, v;

        for (r = 0; r < 3; r++) {           /* Q8 to Q7, halves away from 0; the row back to 128 on the diagonal */
            sum = 0;
            for (c = 0; c < 3; c++) {
                v = m[r * 3 + c];
                v = v >= 0 ? (v + 1) / 2 : -((-v + 1) / 2);
                out[r * 3 + c] = v;
                sum += v;
            }
            out[r * 3 + r] += 128 - sum;
        }
        for (i = 0; i < 9; i++)
            wr16(ispb + OFF_MATRIX + 2 * i, (u32)(u16)(s16)out[i]);
        wr16(ispb + OFF_PRESENT, rd16(ispb + OFF_PRESENT) | OMK_ISPB_P_MATRIX);
    }
}

/* A black-and-white preset (with its edits: a black-and-white filter writes equal rows): the rule core uses for the look's
 * grain - no colour grain on it (omk_ispb.h) */
int edits_mono(const u8 *ispb)
{
    return omk_ispb_mono(ispb);
}

u32 edits_changed(const edits_t *e)
{
    return (e->tone[0] || e->tone[1] || e->tone[2] ? 1u << OMK_EDIT_TONE : 0) | (e->vivid_set ? 1u << OMK_EDIT_VIVID : 0) |
           (e->filter ? 1u << OMK_EDIT_LEVEL : 0) | (e->grain || e->tile[0] ? 1u << OMK_EDIT_GRAIN : 0) |
           (e->optics[OMK_OPTICS_BLOOM] || e->optics[OMK_OPTICS_HALATION] ? 1u << OMK_EDIT_OPTICS : 0);
}

void edits_reset(edits_t *e, u32 group)
{
    u32 i;

    if (group == OMK_EDIT_TONE || group == OMK_EDIT_GROUPS)
        e->tone[0] = e->tone[1] = e->tone[2] = 0;
    if (group == OMK_EDIT_VIVID || group == OMK_EDIT_GROUPS) {
        for (i = 0; i < 12; i++)
            e->vivid[i] = 0;
        e->vivid_set = 0;
    }
    if (group == OMK_EDIT_LEVEL || group == OMK_EDIT_GROUPS)
        e->filter = e->level = 0;
    if (group == OMK_EDIT_GRAIN || group == OMK_EDIT_GROUPS) {   /* OMK_EDIT_GROUPS: every edit, the grain too */
        e->grain = 0;
        for (i = 0; i < GRAIN_FILE; i++)
            e->tile[i] = 0;
    }
    if (group == OMK_EDIT_OPTICS || group == OMK_EDIT_GROUPS)    /* ... and the optics rows */
        for (i = 0; i < OMK_OPTICS_ROWS; i++)
            e->optics[i] = 0;
}

static s32 clamp(s32 v, s32 lo, s32 hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

int edits_event(edits_t *e, u32 arg)
{
    u32 g = OMK_EDIT_GROUP_OF(arg), i = OMK_EDIT_INDEX_OF(arg);
    s32 v = OMK_EDIT_VALUE_OF(arg);

    if (g >= OMK_EDIT_GROUPS)
        return 0;
    if (OMK_EDIT_IS_RESET(arg)) {
        if (!(edits_changed(e) >> g & 1))
            return 0;
        edits_reset(e, g);
        return 1;
    }
    if (g == OMK_EDIT_TONE && i < 3) {
        v = clamp(v, -7, 7);
        if (e->tone[i] == v)
            return 0;
        e->tone[i] = (signed char)v;
        return 1;
    }
    if (g == OMK_EDIT_VIVID && i < 12) {
        v = clamp(v, -5, 5);
        if (e->vivid[i] == v)
            return 0;
        e->vivid[i] = (signed char)v;
        e->vivid_set = (u16)(v ? e->vivid_set | 1u << i : e->vivid_set & ~(1u << i));
        return 1;
    }
    if (g == OMK_EDIT_LEVEL && i == 0) {
        v = clamp(v, 0, 8);
        if (e->filter == v)
            return 0;
        e->filter = (u8)v;
        if (!v)
            e->level = 0;                   /* no filter: no level either (the preset's own weights) */
        else if (!e->level)
            e->level = 2;                   /* a filter chosen first: the middle level */
        return 1;
    }
    if (g == OMK_EDIT_LEVEL && i == 1) {
        v = clamp(v, 1, 3);
        if (e->level == v || !e->filter)
            return 0;                       /* the level only with a filter */
        e->level = (u8)v;
        return 1;
    }
    return 0;
}

void edits_show(const u8 *base, const u8 *work, const edits_t *e, u32 groups, omk_look_edits *out)
{
    u32 i, k, h0, hw, centre;
    s32 a, y, cb, cr, rgb[3];

    out->groups = groups;
    out->edited = edits_changed(e) & groups;
    for (i = 0; i < 3; i++)
        out->tone[i] = e->tone[i];
    for (k = 0; k < 64; k++) {
        u32 v = at16(work + OFF_TONE, at16(work + OFF_GAMMA_G, curve_at[k]));

        out->curve[k] = (u8)(v >= 1023 ? 255 : v * 255 / 1023);
    }
    out->vivid_mask = (u16)(rd16(base + OFF_MASK) & 0x0fff);
    for (i = 0; i < 12; i++) {
        const u8 *r = base + OFF_REC + REC * i;

        a = (s16)rd16(r + REC_SAT);
        a = a >= 0 ? (a + SAT_STEP / 2) / SAT_STEP : -((-a + SAT_STEP / 2) / SAT_STEP);
        out->vivid_base[i] = (signed char)clamp(a, -127, 127);
        out->vivid[i] = e->vivid[i];
        h0 = rd16(r);
        hw = rd16(r + 2);
        centre = (h0 + hw / 2) % TURN;
        out->vivid_hue[i] = (u16)(centre * 360 / TURN);
        if (!(groups >> OMK_EDIT_VIVID & 1)) {
            /* Color/Vivid is not shown: nothing draws the vertices, so their colours (two table sines and five
             * divisions each, at every publication) are left out, 0. The mask, the hues and the sectors' own
             * saturation above stay as ever: the driver orders the sectors by the hues whatever is shown */
            out->vivid_rgb[i][0] = out->vivid_rgb[i][1] = out->vivid_rgb[i][2] = 0;
            continue;
        }
        /* the vertex's colour: Y 140, chroma 56 at that hue (h = atan2(Cr, Cb)), BT.601 */
        k = centre * 64 / TURN;
        cb = 56 * sin64[(k + 16) % 64] / 4096;
        cr = 56 * sin64[k] / 4096;
        y = 140;
        rgb[0] = y + cr * 1402 / 1000;
        rgb[1] = y - (cb * 344 + cr * 714) / 1000;
        rgb[2] = y + cb * 1772 / 1000;
        for (k = 0; k < 3; k++)
            out->vivid_rgb[i][k] = (u8)clamp(rgb[k], 0, 255);
    }
    out->filter = e->filter;
    out->level = e->level ? e->level : 2;
}

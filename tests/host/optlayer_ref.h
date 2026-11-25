/* The first version of the optics layer's arithmetic (optlayer.h as the camera first ran it), kept for the host test as
 * the reference the faster code must give the same values as: the read of a picture row into the sums, the means, the
 * extended box over a row or a column (edges clamped in the loop), the Gaussian and the sum of two, the layer word.
 * Copied as it was, the names prefixed ref_, its state in ref_t. Uses optlayer.h's plan, curves and ol_par_t. */
#ifndef OMK_OPTLAYER_REF_H
#define OMK_OPTLAYER_REF_H

typedef struct {
    u32 lw, lh;
    u8 *y;
    u16 *pb, *ph, *pc;
    u16 *tmp;                               /* max(lw, lh) + 1 */
    u8 *nrow, *cwp;
    const u16 *lin;
    const u8 *enc;
    u32 shift;
} ref_t;

static inline void ref_read_row(ref_t *o, const ol_par_t *p, const volatile u32 *row, u32 cy)
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

static inline void ref_sources(ref_t *o, const ol_par_t *p)
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

static inline void ref_box(u16 *v, u32 n, u32 step, u32 r, u32 alpha, u16 *tmp)
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

static inline void ref_gauss(ref_t *o, u16 *plane, u32 sigma)
{
    u32 r, a, x, y, k;

    if (sigma < 64)
        return;
    if (sigma > OL_SIGMA_MAX)
        sigma = OL_SIGMA_MAX;
    ol_box_plan(sigma, &r, &a);
    for (k = 0; k < OL_BOX_PASSES; k++) {
        for (y = 0; y < o->lh; y++)
            ref_box(plane + y * o->lw, o->lw, 1, r, a, o->tmp);
        for (x = 0; x < o->lw; x++)
            ref_box(plane + x, o->lh, o->lw, r, a, o->tmp);
    }
}

static inline void ref_blur2(ref_t *o, u16 *plane, u32 s1, u32 s2, u32 mix)
{
    u32 i, n = o->lw * o->lh;

    if (!mix || !s2) {
        ref_gauss(o, plane, s1);
        return;
    }
    if (mix > 256)
        mix = 256;
    for (i = 0; i < n; i++)
        o->pc[i] = plane[i];
    ref_gauss(o, plane, s1);
    ref_gauss(o, o->pc, s2);
    for (i = 0; i < n; i++)
        plane[i] = (u16)(((u32)plane[i] * (256 - mix) + (u32)o->pc[i] * mix + 128) >> 8);
}

static inline u32 ref_word(const ref_t *o, const ol_par_t *p, u32 i)
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

#endif

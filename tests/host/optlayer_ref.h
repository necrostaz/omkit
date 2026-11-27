/* The first version of the optics layer's arithmetic (optlayer.h as the camera first ran it), kept for the host test as
 * the reference the faster code must give the same values as: the read of a picture row into the sums, the means, the
 * extended box over a row or a column (edges clamped in the loop), the Gaussian and the sum of two, the layer word; and
 * card170's Bloom's colour (ref_colour, ref_bloom_chroma). Copied as it was, the names prefixed ref_, its state in ref_t
 * (the colour's over ol_t). Uses optlayer.h's plan, curves and ol_par_t. */
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

/* card170's Bloom's colour (optlayer.h up to card172), kept for the host test as the reference the colour of card173 (cells
 * of 8 x 8, one Gaussian of the four, looked up at the words' pixels) must stay near: four sums a cell of the tail's
 * plane (tw x th, OL_TAIL x OL_TAIL layer pixels), each blurred by sc, the ratio, enlarged bilinearly into qpc (Cb | Cr
 * << 8 a pixel); the word's chroma with it. Over o's layer (pb, y, pc, wq, the work rows), the planes q4 (4 tw th) and
 * qpc (lw lh) the caller's */
static inline void ref_colour(ol_t *o, u16 *q4, u16 *qpc, u32 s, u32 sc)
{
    u32 lw = o->lw, tw = o->tw, th = o->th, n4 = tw * th, ty, tx, y, y0, y1, x, x1, i, v, q, n, k, sa, sb, sy, sd, jy, qy,
        a, b, w0, w1, j, tn = (lw + OL_TAIL - 1) / OL_TAIL, va, vb, vc, b5, b7, col[512];
    u16 *qa = q4, *qb = qa + n4, *qyp = qb + n4, *qd = qyp + n4, *out, *end;
    s32 ra, rb, d, yy;

    for (ty = 0; ty < th; ty++) {           /* the sums' means */
        y0 = ty * OL_TAIL;
        y1 = y0 + OL_TAIL < o->lh ? y0 + OL_TAIL : o->lh;
        for (tx = 0, x = 0; tx < tw; tx++, x += OL_TAIL) {
            i = ty * tw + tx;
            if (x >= lw) {
                qa[i] = qa[i - 1];
                qb[i] = qb[i - 1];
                qyp[i] = qyp[i - 1];
                qd[i] = qd[i - 1];
                continue;
            }
            x1 = x + OL_TAIL < lw ? x + OL_TAIL : lw;
            sa = sb = sy = sd = 0;
            for (y = y0; y < y1; y++)
                for (k = y * lw + x; k < y * lw + x1; k++) {
                    if (!o->pb[k])
                        continue;
                    v = o->pb[k] * o->wq[o->y[k]] >> 8;
                    q = o->pc[k];
                    sd += v;
                    sy += v * o->y[k] >> 8;
                    sa += v * (q & 0xff) >> 8;
                    sb += v * (q >> 8) >> 8;
                }
            n = (x1 - x) * (y1 - y0);
            if (n == OL_TAIL * OL_TAIL) {
                sa = (sa + 8) >> 4;
                sb = (sb + 8) >> 4;
                sy = (sy + 8) >> 4;
                sd = (sd + 8) >> 4;
            } else {
                sa = (sa + n / 2) / n;
                sb = (sb + n / 2) / n;
                sy = (sy + n / 2) / n;
                sd = (sd + n / 2) / n;
            }
            qa[i] = (u16)(sa > OL_ONE ? OL_ONE : sa);
            qb[i] = (u16)(sb > OL_ONE ? OL_ONE : sb);
            qyp[i] = (u16)(sy > OL_ONE ? OL_ONE : sy);
            qd[i] = (u16)(sd > OL_ONE ? OL_ONE : sd);
        }
    }
    for (k = 0; k < 4; k++)                 /* each blurred */
        ol_plane_blur(o, q4 + k * n4, sc);
    for (i = 0; i < n4; i++) {              /* the ratio */
        d = qd[i];
        yy = qyp[i];
        yy += OL_COL_SOFT;
        ra = (2 * (s32)qa[i] - d) * (s32)s / (4 * yy);
        rb = (2 * (s32)qb[i] - d) * (s32)s / (4 * yy);
        qa[i] = (u16)(128 + (ra < -127 ? -127 : ra > 127 ? 127 : ra));
        qb[i] = (u16)(128 + (rb < -127 ? -127 : rb > 127 ? 127 : rb));
    }
    for (y = 0, jy = 0, qy = 0; y < o->lh; y++) {   /* enlarged: weights 3:5, 1:7, 7:1, 5:3, edges held */
        if (qy < 2) {
            a = jy ? jy - 1 : 0;
            b = jy;
            w0 = qy ? 1 : 3;
        } else {
            a = jy;
            b = jy + 1 < th ? jy + 1 : jy;
            w0 = qy == 2 ? 7 : 5;
        }
        w1 = 8 - w0;
        for (j = 0; j < tn; j++)
            col[j] = ((u32)qa[a * tw + j] | (u32)qb[a * tw + j] << 16) * w0 +
                     ((u32)qa[b * tw + j] | (u32)qb[b * tw + j] << 16) * w1;
        out = qpc + y * lw;
        end = out + lw;
        for (j = 0, va = col[0]; j < tn && out != end; j++, va = vb) {
            vb = col[j];
            vc = j + 1 < tn ? col[j + 1] : vb;
            b5 = (vb << 2) + vb;
            b7 = b5 + vb + vb;
            v = ((va << 1) + va + b5 + 0x00200020u) >> 6;
            *out++ = (u16)((v & 0xff) | (v >> 8 & 0xff00));
            if (out == end)
                break;
            v = (va + b7 + 0x00200020u) >> 6;
            *out++ = (u16)((v & 0xff) | (v >> 8 & 0xff00));
            if (out == end)
                break;
            v = (b7 + vc + 0x00200020u) >> 6;
            *out++ = (u16)((v & 0xff) | (v >> 8 & 0xff00));
            if (out == end)
                break;
            v = (b5 + (vc << 1) + vc + 0x00200020u) >> 6;
            *out++ = (u16)((v & 0xff) | (v >> 8 & 0xff00));
        }
        if (++qy == OL_TAIL) {
            qy = 0;
            jy++;
        }
    }
}

/* card170's word chroma with Bloom's colour: the chroma bytes w + Bloom's code yb times q (Cb | Cr << 8, 128 + Q7) */
static inline u32 ref_bloom_chroma(const ol_par_t *p, u32 w, u32 yb, u32 q)
{
    s32 cb = (s32)(w >> (8 * p->pcb) & 0xff) + (((s32)yb * ((s32)(q & 0xff) - 128) + 64) >> 7);
    s32 cr = (s32)(w >> (8 * p->pcr) & 0xff) + (((s32)yb * ((s32)(q >> 8) - 128) + 64) >> 7);

    return ol_clamp8(cb) << (8 * p->pcb) | ol_clamp8(cr) << (8 * p->pcr);
}

#endif

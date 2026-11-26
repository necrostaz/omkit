/* The OM5M2 ISPB block (OMKit/docs/formats/isp.md, section 5): its layout and the rules shared by core and the modules
 * that make or read blocks. The one place for them: a module includes this header rather than keeping its own copy */
#ifndef OMK_ISPB_H
#define OMK_ISPB_H

#include "omk_types.h"

#define OMK_ISPB_LAYOUT 0                   /* u16: 1 - OM5M2 layout 1 */
#define OMK_ISPB_PRESENT 2                  /* u16: the parts the block replaces, OMK_ISPB_P_* */
#define OMK_ISPB_P_MATRIX 1                 /* the matrix */
#define OMK_ISPB_P_GAMMA 2                  /* the gamma R, G, B */
#define OMK_ISPB_P_TONE 4                   /* the tone on Y */
#define OMK_ISPB_P_GAINS 8                  /* the Cb/Cr gains */
#define OMK_ISPB_P_SECTORS 16               /* the sector records and profiles */
#define OMK_ISPB_MATRIX 8                   /* s16[9], Q7, rows sum to 128 */
#define OMK_ISPB_CB 28                      /* u16 k_Cb, Q8 */
#define OMK_ISPB_CR 30                      /* u16 k_Cr, Q8 */
#define OMK_ISPB_GAMMA 32                   /* u16[3][256]: R, G, B */
#define OMK_ISPB_TONE 1568                  /* u16[256] */
#define OMK_ISPB_SECTOR_MASK 2080           /* u16: the sectors enabled */
#define OMK_ISPB_SECTORS 2088               /* 12 records of 24 bytes */
#define OMK_ISPB_PROFILES 2376              /* s16[12][7][16] */
#define OMK_ISPB_SIZE 5064
#define OMK_ISPB_MONO_ROWS 2                /* matrix rows this close (Q7 steps, every column) make a black-and-white block */

static inline u32 omk_ispb_u16(const u8 *b, u32 at)
{
    return (u32)b[at] | (u32)b[at + 1] << 8;
}

/* 1 - a black-and-white block, 0 - colour. Black and white when either holds:
 *  - the block sets the Cb/Cr gains (OMK_ISPB_P_GAINS) and one of them is 0 - no colour difference survives;
 *  - the block sets the matrix (OMK_ISPB_P_MATRIX) and its rows differ by at most OMK_ISPB_MONO_ROWS in every column -
 *    R, G and B come out as one mix of the camera's channels, as a black-and-white filter writes them.
 * A part the block does not set is not looked at (its field may hold 0); a block with neither part keeps the camera's
 * matrix and gains of the picture mode it replaces - colour, 0. A tint from the gamma curves or the sectors does not make
 * a black-and-white block colour. The 22 reference look presets: TRIX 0 (gains 0 as well), DOUBLEX 1 (toned by its gamma
 * curves), the colour ones 91..130 */
static inline int omk_ispb_mono(const u8 *b)
{
    u32 present = omk_ispb_u16(b, OMK_ISPB_PRESENT);
    int i, j, k, d, most = 0;

    if ((present & OMK_ISPB_P_GAINS) && (!omk_ispb_u16(b, OMK_ISPB_CB) || !omk_ispb_u16(b, OMK_ISPB_CR)))
        return 1;
    if (!(present & OMK_ISPB_P_MATRIX))
        return 0;
    for (i = 0; i < 3; i++)
        for (j = i + 1; j < 3; j++)
            for (k = 0; k < 3; k++) {
                d = (int)(s16)omk_ispb_u16(b, OMK_ISPB_MATRIX + 6 * i + 2 * k) -
                    (int)(s16)omk_ispb_u16(b, OMK_ISPB_MATRIX + 6 * j + 2 * k);
                d = d < 0 ? -d : d;
                most = d > most ? d : most;
            }
    return most <= OMK_ISPB_MONO_ROWS;
}

/* The block's white: the code of Y (0..255) a blown pixel gets - the tone curve's output for the gamma's largest input,
 * where the look's white ends (a film's print white: 204..255 among the reference presets). The gamma curves at their
 * last node (the input clipped at the full scale, so white is node 255 on R, G and B; the matrix keeps white, its rows
 * sum to 128), their Y (BT.601), the tone curve at it (node i at Y = i / 256 of the full scale; above node 255 its last
 * segment continued), 10 bits to 8. The sectors' luma addition is left out (at white within 3 codes on the presets).
 * 0 - not known: the block does not set both the gamma and the tone (the picture mode's own curves are in force, and
 * the camera's contrast and gradation with them) */
static inline u32 omk_ispb_white(const u8 *b)
{
    u32 present = omk_ispb_u16(b, OMK_ISPB_PRESENT), y, q, j, t0, t1;
    s32 t;

    if ((present & (OMK_ISPB_P_GAMMA | OMK_ISPB_P_TONE)) != (OMK_ISPB_P_GAMMA | OMK_ISPB_P_TONE))
        return 0;
    y = (299 * omk_ispb_u16(b, OMK_ISPB_GAMMA + 2 * 255) + 587 * omk_ispb_u16(b, OMK_ISPB_GAMMA + 512 + 2 * 255) +
         114 * omk_ispb_u16(b, OMK_ISPB_GAMMA + 1024 + 2 * 255) + 500) / 1000;   /* 0..4095 */
    q = y * 4096 / 4095;                    /* the tone's position, x16 (node i at 16 i) */
    j = q >> 4 < 254 ? q >> 4 : 254;
    t0 = omk_ispb_u16(b, OMK_ISPB_TONE + 2 * j);
    t1 = omk_ispb_u16(b, OMK_ISPB_TONE + 2 * j + 2);
    t = ((s32)t0 * 16 + ((s32)t1 - (s32)t0) * (s32)(q - 16 * j) + 8) / 16;
    t = t < 0 ? 0 : t > 1023 ? 1023 : t;
    return t ? ((u32)t * 255 + 511) / 1023 : 1;
}

#endif

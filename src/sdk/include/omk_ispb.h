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

#endif

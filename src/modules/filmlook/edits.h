/* FILMLOOK: the user's edits of a preset (the look's editors, API 16) applied to its ISPB block. Integer arithmetic only:
 * the same steps are repeated by the computer's model, so what the model shows is what the camera gets */
#ifndef FILMLOOK_EDITS_H
#define FILMLOOK_EDITS_H

#include "omk_api.h"
#include "omk_ispb.h"

#define ISPB_SIZE OMK_ISPB_SIZE
#define EDITS_FORMAT 3                      /* 3: with the optics rows' strengths; 2: with the grain (FILMLOOK 1.5), taken with
                                               the optics rows at 0; 1: Color/Vivid a shift from the preset's saturation (1.3),
                                               taken with the grain Off; 0 - its replacement (1.2), not taken */
#define GRAIN_FILE 13                       /* a grain container's 8.3 file name with its terminator */

typedef struct {
    signed char tone[3];                    /* shadows, middle, highlights: -7..7, 0 - the preset's curve */
    signed char vivid[12];                  /* a sector's saturation shift from the preset's, -5..5 (0.05 a step) */
    u8 filter, level;                       /* the black-and-white filter 1..8 and its level 1..3; filter 0 - the preset's */
    u8 format;                              /* EDITS_FORMAT: what the values mean (the store keeps them) */
    u16 vivid_set;                          /* bit i: sector i is shifted (vivid[i] not 0) */
    /* format 2 */
    u8 grain;                               /* the grain's strength: 0 Off, 1 Low, 2 Med, 3 High */
    char tile[GRAIN_FILE];                  /* the grain tile's container file; "" - the module's default tile */
    /* format 3 (in the two bytes a format 2 record kept as padding: the record's size is the same) */
    u8 optics[OMK_OPTICS_ROWS];             /* the optics rows' strengths 0..OMK_OPTICS_MAX: Bloom, Halation; 0 - off */
} edits_t;
#define EDITS_V1_SIZE 20                    /* sizeof(edits_t) of format 1: the fields up to vivid_set */

/* The edits into a copy of the preset's ISPB block */
void edits_apply(u8 *ispb, const edits_t *e);
/* What the editors show: the values (Color/Vivid: the shifts, 0 at the start), the curve of the edited block, the
 * vertices of the preset's sectors and their own saturation. groups: OMK_EDIT_* bits shown; the vertices' colours
 * (vivid_rgb) only with Color/Vivid among them, 0 otherwise */
void edits_show(const u8 *base, const u8 *work, const edits_t *e, u32 groups, omk_look_edits *out);
/* 1 - a black-and-white block (the preset with its edits): omk_ispb_mono, the one rule */
int edits_mono(const u8 *ispb);
/* OMK_EDIT_* bits of the groups the user changed */
u32 edits_changed(const edits_t *e);
/* A group back to the preset (OMK_EDIT_*); OMK_EDIT_GROUPS - all */
void edits_reset(edits_t *e, u32 group);
/* One change from an editor (OMK_EDIT_EVENT): 1 applied, 0 nothing to do (out of range, the same value) */
int edits_event(edits_t *e, u32 arg);

#endif

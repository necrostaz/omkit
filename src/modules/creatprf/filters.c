/* CREATPRF: the alternative colour filters of monochrome profiles (mono_filters = alternative).
 *
 * The alternative set is softer than the camera's own for RED, MAGENTA, BLUE and CYAN: its level 3 is the camera's level 2,
 * its level 2 lies halfway between the camera's levels 1 and 2. In each row the first two values are the midpoint rounded
 * half away from zero and the third one makes the row sum 256 again, as the camera's rows do (the brightness of grey stays).
 * No camera data is kept here: the module reads the camera's matrices through core (prof_mono_matrix_get). Kept apart from
 * creatprf.c so that a host test can run it. */
#include "filters.h"

void alt_level2(const s16 *level1, const s16 *level2, s16 *out)
{
    u32 r, c;
    s32 s, row;

    for (r = 0; r < 3; r++) {
        row = 0;
        for (c = 0; c < 2; c++) {
            s = level1[3 * r + c] + level2[3 * r + c];
            out[3 * r + c] = (s16)((s >= 0 ? s + 1 : s - 1) / 2);   /* C division truncates: half away from zero */
            row += out[3 * r + c];
        }
        out[3 * r + 2] = (s16)(256 - row);
    }
}

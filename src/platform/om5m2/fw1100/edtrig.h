/* The graphic editors' sines (editor.c): angles in steps of 2.5 degrees, 144 to the turn, clockwise from the top; values
 * in Q14. Only a quarter of the wave is kept, 37 values: the renderer copies them to its stack once a frame and takes
 * every sine from there - it runs on CPU 1, where the image's constants are read through the uncached alias (a bus access
 * each), and a drawing needs two sines a point, up to 4000 a frame; its own stack is in its own cache.
 * A sine by the wave's symmetry, the angle wrapped by comparing and subtracting: no division (the CPU's divide is slow
 * and the angles are at most a few turns). The values are those of the full table of 144 this replaces
 * (tests/host/test_edtrig.c keeps that table and compares every angle).
 *
 * Plain C, no firmware address: the host test runs the very code. The includer defines s16, s32, u32 (omk_types.h). */
#ifndef OMK_EDTRIG_H
#define OMK_EDTRIG_H

#define EDP_STEPS 144                       /* the turn */
#define EDP_QUARTER (EDP_STEPS / 4)
#define EDP_SIN_N (EDP_QUARTER + 1)         /* sin(0) .. sin(90 degrees) */

/* sin(2.5 * i degrees) in Q14, i = 0..36 */
static const s16 edp_sin_q[EDP_SIN_N] = {
    0, 715, 1428, 2139, 2845, 3546, 4240, 4927, 5604, 6270, 6924, 7565,
    8192, 8803, 9397, 9974, 10531, 11069, 11585, 12080, 12551, 12998, 13421, 13818,
    14189, 14533, 14849, 15137, 15396, 15626, 15826, 15996, 16135, 16244, 16322, 16368,
    16384,
};

/* An angle into 0..143. The loop runs at most three times: anything from four turns up (never drawn; a damaged value)
 * takes the one division instead */
static inline u32 edp_wrap(u32 a)
{
    if (a >= 4 * EDP_STEPS)
        a %= EDP_STEPS;
    while (a >= EDP_STEPS)
        a -= EDP_STEPS;
    return a;
}

/* q: the quarter table (the caller's copy of edp_sin_q) */
static inline s32 edp_sin(const s16 *q, u32 a)
{
    a = edp_wrap(a);
    if (a <= EDP_QUARTER)
        return q[a];
    if (a <= 2 * EDP_QUARTER)
        return q[2 * EDP_QUARTER - a];
    if (a <= 3 * EDP_QUARTER)
        return -q[a - 2 * EDP_QUARTER];
    return -q[EDP_STEPS - a];
}

static inline s32 edp_cos(const s16 *q, u32 a)
{
    return edp_sin(q, a + EDP_QUARTER);
}

#endif

/* Host test of the graphic editors' sines (src/platform/om5m2/fw1100/edtrig.h, the code the renderer runs): the quarter
 * table with its symmetry and the angle wrapped without a division give, for every angle, the value of the full table of
 * 144 the renderer had before (kept here), so every point of every drawing is where it was. Prints "ok". */
#include <stdio.h>
#include "omk_types.h"
#include "edtrig.h"

/* sin(2.5 * i degrees) in Q14: the renderer's table up to core 1.29 */
static const s16 full[144] = {
    0, 715, 1428, 2139, 2845, 3546, 4240, 4927, 5604, 6270, 6924, 7565,
    8192, 8803, 9397, 9974, 10531, 11069, 11585, 12080, 12551, 12998, 13421, 13818,
    14189, 14533, 14849, 15137, 15396, 15626, 15826, 15996, 16135, 16244, 16322, 16368,
    16384, 16368, 16322, 16244, 16135, 15996, 15826, 15626, 15396, 15137, 14849, 14533,
    14189, 13818, 13421, 12998, 12551, 12080, 11585, 11069, 10531, 9974, 9397, 8803,
    8192, 7565, 6924, 6270, 5604, 4927, 4240, 3546, 2845, 2139, 1428, 715,
    0, -715, -1428, -2139, -2845, -3546, -4240, -4927, -5604, -6270, -6924, -7565,
    -8192, -8803, -9397, -9974, -10531, -11069, -11585, -12080, -12551, -12998, -13421, -13818,
    -14189, -14533, -14849, -15137, -15396, -15626, -15826, -15996, -16135, -16244, -16322, -16368,
    -16384, -16368, -16322, -16244, -16135, -15996, -15826, -15626, -15396, -15137, -14849, -14533,
    -14189, -13818, -13421, -12998, -12551, -12080, -11585, -11069, -10531, -9974, -9397, -8803,
    -8192, -7565, -6924, -6270, -5604, -4927, -4240, -3546, -2845, -2139, -1428, -715,
};

int main(void)
{
    s16 q[EDP_SIN_N];
    u32 a, i, failed = 0;

    for (i = 0; i < EDP_SIN_N; i++)         /* the renderer's copy on its stack */
        q[i] = edp_sin_q[i];
    for (i = 0; i < 4000 + 16; i++) {
        a = i < 4000 ? i : 0xffffffffu - (i - 4000) * 0x01234567u;   /* every angle drawn, then damaged values */
        if (edp_wrap(a) != a % 144 || edp_sin(q, a) != full[a % 144] ||
            (a < 0xffffff00u && edp_cos(q, a) != full[(a + 36) % 144])) {
            fprintf(stderr, "angle %u: sine %d (full table %d), wrapped %u\n", a, (int)edp_sin(q, a), full[a % 144], edp_wrap(a));
            failed++;
        }
    }
    if (failed)
        return 1;
    printf("ok\n");
    return 0;
}

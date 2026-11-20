/* Host test of CREATPRF's alternative filters (src/modules/creatprf/filters.c): reads groups of 18 numbers from stdin -
 * a filter's level 1 and level 2 matrices - and prints the computed level 2, nine numbers a line */
#include <stdio.h>
#include "filters.h"

int main(void)
{
    s16 a[9], b[9], out[9];
    int i, v;

    for (;;) {
        for (i = 0; i < 18; i++) {
            if (scanf("%d", &v) != 1)
                return 0;
            if (i < 9)
                a[i] = (s16)v;
            else
                b[i - 9] = (s16)v;
        }
        alt_level2(a, b, out);
        for (i = 0; i < 9; i++)
            printf("%d ", out[i]);
        printf("\n");
    }
}

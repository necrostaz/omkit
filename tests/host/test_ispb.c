/* Host test of the black-and-white rule (sdk omk_ispb.h) and FILMLOOK's use of it (src/modules/filmlook/edits.c): reads
 * lines from stdin - the parts present, Cb, Cr, the nine matrix numbers, a black-and-white filter 0..8 and its level - and
 * prints omk_ispb_mono and edits_mono of the block with that filter applied (FILMLOOK's edits_apply), two numbers a line */
#include <stdio.h>
#include <string.h>
#include "edits.h"

static u8 block[ISPB_SIZE];

static void put16(u32 at, int v)
{
    block[at] = (u8)v;
    block[at + 1] = (u8)((u32)v >> 8);
}

int main(void)
{
    int v[14], i;
    edits_t e;

    for (;;) {
        for (i = 0; i < 14; i++)
            if (scanf("%d", &v[i]) != 1)
                return 0;
        memset(block, 0, sizeof(block));
        put16(OMK_ISPB_LAYOUT, 1);
        put16(OMK_ISPB_PRESENT, v[0]);
        put16(OMK_ISPB_CB, v[1]);
        put16(OMK_ISPB_CR, v[2]);
        for (i = 0; i < 9; i++)
            put16(OMK_ISPB_MATRIX + 2 * i, v[3 + i]);
        memset(&e, 0, sizeof(e));
        e.filter = (u8)v[12];
        e.level = (u8)v[13];
        printf("%d ", omk_ispb_mono(block));
        edits_apply(block, &e);
        printf("%d\n", edits_mono(block));
    }
}

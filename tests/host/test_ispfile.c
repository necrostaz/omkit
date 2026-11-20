/* Host test of FILMLOOK's .isp file code (src/modules/filmlook/ispfile.c):
 *  - the table CRC-32 gives what the bit-wise loop it replaced gave (the loop is kept here as the reference) and what
 *    zlib gives, on fixed vectors, on pseudo-random data of every length and alignment, in one piece or in several;
 *  - every preset file named on the command line passes isp_check with an ISPB section of the OM5M2 layout, its CRC by
 *    the table equals the reference's (the store keys a preset's edits by it), and a changed byte anywhere in a section
 *    or in the header is caught.
 * Run: test_ispfile [<preset.ISP> ...]; prints "ok" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ispfile.h"
#include "omk_ispb.h"

static int failed;

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);             \
            failed++;                                                           \
        }                                                                       \
    } while (0)

/* The bit-wise CRC-32 FILMLOOK used before the table, unchanged */
static u32 crc_bitwise(u32 crc, const u8 *p, u32 n)
{
    u32 k;

    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (k = 0; k < 8; k++)
            crc = crc & 1 ? crc >> 1 ^ 0xedb88320u : crc >> 1;
    }
    return ~crc;
}

static u32 rnd_state = 0x12345678u;

static u32 rnd(void)                        /* xorshift32: the same data in every run */
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

static void test_crc(void)
{
    static u8 data[16384 + 8];
    u32 i, n, at, cut, whole;

    CHECK(isp_crc32(0, (const u8 *)"", 0) == 0);
    CHECK(isp_crc32(0, (const u8 *)"123456789", 9) == 0xcbf43926u);     /* the check value of CRC-32 (zlib) */
    CHECK(isp_crc32(0, (const u8 *)"a", 1) == 0xe8b7be43u);
    for (i = 0; i < sizeof(data); i++)
        data[i] = (u8)(rnd() >> 11);
    for (n = 0; n <= 300; n++)              /* every short length at every alignment */
        for (at = 0; at < 8; at++)
            CHECK(isp_crc32(0, data + at, n) == crc_bitwise(0, data + at, n));
    for (i = 0; i < 200; i++) {             /* file-sized pieces, whole and cut in two at a random place */
        n = rnd() % 16385;
        at = rnd() % 8;
        cut = n ? rnd() % (n + 1) : 0;
        whole = crc_bitwise(0, data + at, n);
        CHECK(isp_crc32(0, data + at, n) == whole);
        CHECK(isp_crc32(isp_crc32(0, data + at, cut), data + at + cut, n - cut) == whole);
        CHECK(isp_crc32(0x5a5a5a5au, data + at, n) == crc_bitwise(0x5a5a5a5au, data + at, n));
    }
}

static void test_file(const char *path)
{
    static u8 b[16384];
    const u8 *ispb = 0, *cani = 0, *ispb2 = 0;
    u32 n, ispb_n = 0, cani_n = 0, ispb2_n = 0, i, pos[4];
    const char *why;
    FILE *f = fopen(path, "rb");

    if (!f) {
        fprintf(stderr, "%s: cannot open\n", path);
        failed++;
        return;
    }
    n = (u32)fread(b, 1, sizeof(b), f);
    fclose(f);
    CHECK(n > ISP_HEADER && n < sizeof(b));
    why = isp_check(b, n, &ispb, &ispb_n, &cani, &cani_n);
    if (why) {
        fprintf(stderr, "%s: %s\n", path, why);
        failed++;
        return;
    }
    CHECK(ispb && ispb_n == OMK_ISPB_SIZE);
    CHECK(!cani || (cani > b && cani + cani_n <= b + n));
    /* without the cassette picture asked for: the same ISPB section */
    CHECK(!isp_check(b, n, &ispb2, &ispb2_n, 0, 0) && ispb2 == ispb && ispb2_n == ispb_n);
    CHECK(isp_crc32(0, b, n) == crc_bitwise(0, b, n));
    /* damage: one bit of a byte in the header's title, in the tables, in the last byte of the cassette picture (of the
     * tables without one), in the size */
    pos[0] = ISP_TITLE + 8;
    pos[1] = (u32)(ispb - b) + 100;
    pos[2] = cani ? (u32)(cani - b) + cani_n - 1 : (u32)(ispb - b) + ispb_n - 1;
    pos[3] = 21;
    for (i = 0; i < 4; i++) {
        b[pos[i]] ^= 0x10;
        CHECK(isp_check(b, n, &ispb2, &ispb2_n, &cani, &cani_n) != 0);
        b[pos[i]] ^= 0x10;
    }
    CHECK(isp_check(b, n - 1, &ispb2, &ispb2_n, 0, 0) != 0);
    CHECK(!isp_check(b, n, &ispb2, &ispb2_n, 0, 0));
}

int main(int argc, char **argv)
{
    int i;

    test_crc();
    for (i = 1; i < argc; i++)
        test_file(argv[i]);
    if (!failed)
        printf("ok\n");
    return failed != 0;
}

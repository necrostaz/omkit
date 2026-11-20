/* FILMLOOK: the checks of an .isp file (OMKit/docs/formats/isp.md) and its CRC-32. No API calls: the same code runs in the
 * host test (OMKit/tests/host/test_ispfile.c) */
#ifndef FILMLOOK_ISPFILE_H
#define FILMLOOK_ISPFILE_H

#include "omk_types.h"

#define ISP_HEADER 64                       /* the file's header */
#define ISP_TITLE 32                        /* where the title is in the header (up to 31 characters, zero padded) */

/* CRC-32 (zlib: reflected, polynomial 0xedb88320), a table step per byte. crc: 0 at the start, or the value so far */
u32 isp_crc32(u32 crc, const u8 *p, u32 n);

/* The file's checks: magic, version, platform, size, header CRC, the CRC of every section; gives the ISPB section and,
 * when asked (cani not 0), the cassette picture (CANI; *cani 0 - none in the file). 0, or why not */
const char *isp_check(const u8 *b, u32 n, const u8 **ispb, u32 *ispb_n, const u8 **cani, u32 *cani_n);

#endif

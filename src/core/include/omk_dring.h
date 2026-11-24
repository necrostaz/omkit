/* RAM ring of the debug service (core/dring.c): records in memory that outlives a reset of the camera, two banks - this
 * power-on and the one before. The usual design of a persistent RAM log (a pstore/ramoops-like ring): a header with a magic
 * number and a checksum of its fixed part, records {sync, type, length, number, ms} behind it, the tail moved before a
 * record is written and the head after it, so a reset in the middle leaves only the unfinished record unread.
 * Plain C, no driver calls: the caller holds the lock (dring_put) and gives the memory. */
#ifndef OMK_DRING_H
#define OMK_DRING_H

#include "omk_types.h"

#define DRING_MAGIC 0x444b4d4fu             /* 'OMKD' */
#define DRING_VERSION 1
#define DRING_HEAD 64                       /* bank header bytes */
#define DRING_REC 12                        /* record header bytes */
#define DRING_SYNC 0xa5
#define DRING_BANKS 2

/* Header words */
enum {
    DH_MAGIC, DH_VERSION, DH_BOOT, DH_SIZE, DH_TAIL, DH_HEAD, DH_SEQ, DH_LOST, DH_RUN0, DH_RUN1, DH_BEAT, DH_FLAGS,
    DH_CAUSE, DH_SNAP_MS, DH_CRC, DH_SPARE, DH_WORDS
};
enum { DF_ENDED = 1, DF_NO_CARD = 2, DF_WRITE = 4 };    /* DH_FLAGS */
enum { DR_LOG = 1, DR_SNAP, DR_MARK, DR_MAP, DR_BOOT }; /* record types */

typedef struct {
    volatile u32 *h;                        /* header, DH_WORDS words */
    volatile u8 *d;                         /* data, size bytes (a multiple of 4) */
    u32 size;
} dring_bank;

typedef struct {
    dring_bank bank[DRING_BANKS];
    int cur, prev;                          /* bank indexes; prev -1: no bank of a run before */
} dring;

/* Lays the banks out in the area (two headers, then the two data parts) and picks them: the valid bank with the higher
 * power-on number is the run before, the other one is set up for this run (boot = that number + 1, or 1). area: 4-byte
 * aligned, at least 2 * (DRING_HEAD + 256) bytes. 0, or -1 too small */
int dring_open(dring *r, void *area, u32 size, const char *run, u32 cause, u32 flags);
int dring_valid(const dring_bank *b);
/* One record of two pieces (a + b, n bytes in all, at most a quarter of the bank): 1 written, 0 too long (counted).
 * The caller holds the lock that orders every writer of the bank */
int dring_put(dring_bank *b, u32 type, u32 ms, const void *a, u32 na, const void *p, u32 np);
/* Bytes of the bank's data from position pos (mod size): copies n, two pieces at most */
void dring_copy(const dring_bank *b, u32 pos, void *out, u32 n);
/* Walk of the records from the tail: fn(type, number, ms, data, length, ctx) for each valid one; stops at the first record
 * that is not (a torn end). Returns the records walked. buf: room for the longest record (a quarter of the bank) */
typedef void (*dring_fn)(u32 type, u32 seq, u32 ms, const u8 *data, u32 len, void *ctx);
u32 dring_walk(const dring_bank *b, u8 *buf, u32 max, dring_fn fn, void *ctx);
u32 dring_crc(const dring_bank *b);         /* the checksum of the header's fixed part */

#endif

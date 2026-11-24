/* RAM ring of the debug service (omk_dring.h). Words of the header and the data are written one by one through the
 * volatile pointers the caller gave (on the camera: uncached memory), in the order that keeps the ring readable after a
 * reset at any point: tail first, then the record, then head and the record count. */
#include "omk_core.h"
#include "omk_dring.h"

#define ALIGN4(n) (((n) + 3) & ~(u32)3)

u32 dring_crc(const dring_bank *b)
{
    u32 w[6], i;

    for (i = 0; i < 4; i++)
        w[i] = b->h[DH_MAGIC + i];
    w[4] = b->h[DH_RUN0];
    w[5] = b->h[DH_RUN1];
    return crc32_update(0, w, sizeof(w));
}

int dring_valid(const dring_bank *b)
{
    u32 tail = b->h[DH_TAIL], head = b->h[DH_HEAD];

    return b->h[DH_MAGIC] == DRING_MAGIC && b->h[DH_VERSION] == DRING_VERSION && b->h[DH_SIZE] == b->size &&
           b->h[DH_BOOT] && head - tail <= b->size && !(tail & 3) && !(head & 3) && b->h[DH_CRC] == dring_crc(b);
}

int dring_open(dring *r, void *area, u32 size, const char *run, u32 cause, u32 flags)
{
    u32 bank, i, boot = 0, w[2] = {0, 0};
    u8 *a = area;
    dring_bank *c;

    if (size < 2 * (DRING_HEAD + 256))
        return -1;
    bank = ((size - 2 * DRING_HEAD) / 2) & ~(u32)3;
    for (i = 0; i < DRING_BANKS; i++) {
        r->bank[i].h = (volatile u32 *)(a + i * DRING_HEAD);
        r->bank[i].d = (volatile u8 *)(a + 2 * DRING_HEAD + i * bank);
        r->bank[i].size = bank;
    }
    r->prev = -1;
    for (i = 0; i < DRING_BANKS; i++)
        if (dring_valid(&r->bank[i]) && r->bank[i].h[DH_BOOT] >= boot) {
            boot = r->bank[i].h[DH_BOOT];
            r->prev = i;
        }
    r->cur = r->prev == 0 ? 1 : 0;
    c = &r->bank[r->cur];
    for (i = 0; i < 8 && run && run[i]; i++)
        w[i / 4] |= (u32)(u8)run[i] << (8 * (i % 4));
    c->h[DH_MAGIC] = 0;                     /* not valid while it is being set up */
    for (i = 1; i < DH_WORDS; i++)
        c->h[i] = 0;
    c->h[DH_VERSION] = DRING_VERSION;
    c->h[DH_BOOT] = boot + 1 ? boot + 1 : 1;
    c->h[DH_SIZE] = bank;
    c->h[DH_RUN0] = w[0];
    c->h[DH_RUN1] = w[1];
    c->h[DH_CAUSE] = cause;
    c->h[DH_FLAGS] = flags;
    c->h[DH_MAGIC] = DRING_MAGIC;
    c->h[DH_CRC] = dring_crc(c);
    return 0;
}

/* The record header at data position pos (4-aligned: never split across the end) */
static u32 word_at(const dring_bank *b, u32 pos)
{
    return *(const volatile u32 *)(b->d + (pos % b->size));
}

static void put_word(dring_bank *b, u32 pos, u32 w)
{
    *(volatile u32 *)(b->d + (pos % b->size)) = w;
}

/* Bytes into words: the source may be unaligned, the ring is word-aligned */
static u32 put_bytes(dring_bank *b, u32 pos, const u8 *s, u32 n, u32 *acc, u32 *fill)
{
    u32 i;

    for (i = 0; i < n; i++) {
        *acc |= (u32)s[i] << (8 * *fill);
        if (++*fill == 4) {
            put_word(b, pos, *acc);
            pos += 4;
            *acc = 0;
            *fill = 0;
        }
    }
    return pos;
}

/* The record's CRC-32: its first 12 bytes (sync, type, length, number, ms), then its data */
static u32 rec_crc(u32 w0, u32 seq, u32 ms, const void *a, u32 na, const void *p, u32 np)
{
    u32 w[3];

    w[0] = w0;
    w[1] = seq;
    w[2] = ms;
    return crc32_update(crc32_update(crc32_update(0, w, sizeof(w)), a, na), p, np);
}

/* Under the caller's lock. The CRC costs about as much as the copy into the ring (a table, a byte at a time: some 2000
 * instructions for a line of 192 bytes) */
int dring_put(dring_bank *b, u32 type, u32 ms, const void *a, u32 na, const void *p, u32 np)
{
    u32 n = na + np, need = DRING_REC + ALIGN4(n), head = b->h[DH_HEAD], tail = b->h[DH_TAIL], pos, acc = 0, fill = 0;
    u32 rec, len, w0, seq;

    if (n > 0xffff || need > b->size / 4) {
        b->h[DH_LOST]++;
        return 0;
    }
    while (head - tail + need > b->size) { /* the oldest records make room */
        rec = word_at(b, tail);
        len = rec >> 16;
        if ((rec & 0xff) != DRING_SYNC || head - tail < DRING_REC + ALIGN4(len)) {
            tail = head;                    /* not a record: the ring starts again (never seen; a stray write) */
            break;
        }
        tail += DRING_REC + ALIGN4(len);
    }
    b->h[DH_TAIL] = tail;                   /* 1: before the bytes of the old records are overwritten */
    w0 = DRING_SYNC | (type & 0xff) << 8 | n << 16;
    seq = b->h[DH_SEQ];
    put_word(b, head, w0);
    put_word(b, head + 4, seq);
    put_word(b, head + 8, ms);
    put_word(b, head + 12, rec_crc(w0, seq, ms, a, na, p, np));
    pos = put_bytes(b, head + DRING_REC, a, na, &acc, &fill);
    pos = put_bytes(b, pos, p, np, &acc, &fill);
    if (fill)
        put_word(b, pos, acc);
    b->h[DH_HEAD] = head + need;            /* 3: the record is whole */
    b->h[DH_SEQ]++;
    return 1;
}

void dring_copy(const dring_bank *b, u32 pos, void *out, u32 n)
{
    u32 at = pos % b->size, first = n < b->size - at ? n : b->size - at, i;
    u8 *o = out;

    for (i = 0; i < first; i++)
        o[i] = b->d[at + i];
    for (; i < n; i++)
        o[i] = b->d[i - first];
}

u32 dring_walk(const dring_bank *b, u8 *buf, u32 max, dring_fn fn, void *ctx, u32 *gaps, u32 *skipped)
{
    u32 tail = b->h[DH_TAIL], head = b->h[DH_HEAD], pos, rec, len, seq, ms, count = 0, last = 0, ng = 0, ns = 0;
    int first = 1, in_gap = 0;

    if (head - tail <= b->size) {
        for (pos = tail; head - pos >= DRING_REC && head - pos <= b->size;) {
            rec = word_at(b, pos);
            len = rec >> 16;
            seq = word_at(b, pos + 4);
            ms = word_at(b, pos + 8);
            if ((rec & 0xff) == DRING_SYNC && head - pos >= DRING_REC + ALIGN4(len) && len <= max &&
                (first || seq - last - 1 < 0x80000000u)) {
                dring_copy(b, pos + DRING_REC, buf, len);
                if (rec_crc(rec, seq, ms, buf, len, 0, 0) == word_at(b, pos + 12)) {
                    fn(rec >> 8 & 0xff, seq, ms, buf, len, ctx);
                    count++;
                    first = in_gap = 0;
                    last = seq;
                    pos += DRING_REC + ALIGN4(len);
                    continue;
                }
            }
            ng += !in_gap;                  /* not a whole record here: the next one is looked for a word further */
            in_gap = 1;
            ns += 4;
            pos += 4;
        }
        if (head - pos <= b->size)
            ns += head - pos;               /* a torn last bit shorter than a header */
    }
    if (gaps)
        *gaps = ng;
    if (skipped)
        *skipped = ns;
    return count;
}

/* Module store (omk_store.h). Records are packed: {u16 key, u8 flags, u8 0, u16 length, u16 0}, then the value,
 * aligned to 4. Changing the length of a value moves the record to the end. File: header and persistent records only.
 *
 * A save that fails (no card, no folder, a short write) is tried again by the service loop with a growing pause: twice
 * STORE_DELAY_MS, doubled after every failure up to STORE_RETRY_MAX_MS; a save that succeeds ends the pauses, and so
 * does store_retry_now (power.c: the camera is running again, the card may be back). store_save_now and
 * store_save_all (the card-off gate) always try.
 *
 * A change is saved STORE_DELAY_MS after the last one (a row of changes - a dial being turned - is one write), but no
 * later than STORE_LATEST_MS after the first one not saved yet: a value that keeps changing, each time within the
 * delay, would otherwise never reach the card until the power-off, and a crash or a pulled battery would lose it all.
 *
 * Both slot files have one fixed length - the header and the whole record space, rounded up to a sector, zeros after the
 * records - and a save rewrites the slot file in place (r+b). The firmware's file system then writes the data sectors and
 * the file's directory entry set, where only the modification time and the set checksum change - both in the first 32-byte
 * entry, so in one sector; the allocation bitmap, the FAT, the length, the first cluster and the name stay as they are. A
 * reset at any point of such a save leaves the volume consistent (emulator: every cut point of a save). Removing and
 * making the file again - what saves did before - changes the bitmap, the FAT and the whole entry set in up to ten
 * commands, and a reset between two of them breaks the volume (an entry set half written, a cluster marked free that a
 * file still uses). That path remains only for a slot file that is not there or has another length (a new card, a store
 * of another size, files of an older core): once per card and slot file. */
#include "omk_core.h"
#include "omk_log.h"
#include "omk_store.h"

#define STORE_MAX 8
#define STORE_MAGIC 0x534b4d4f              /* 'OMKS' */
#define STORE_VERSION 1
#define STORE_DELAY_MS 1500
#define STORE_LATEST_MS 12000               /* the longest a change waits while newer ones keep coming: a write every
                                               12 s at worst, 2-4 KB each, while something changes all the time */
#define STORE_RETRY_MAX_MS 60000            /* the longest pause between tries of a failing save */
#define REC_HEAD 8
#define FILE_HEAD 20                        /* magic, version, record count, version number, length, CRC32 */
#define NAME_MAX 8
#define SECTOR 512                          /* a slot file's length is a multiple: a save rewrites whole sectors */

struct omk_store {
    char name[NAME_MAX + 1];
    u8 *mem, *image;                        /* records; file image for writing and reading */
    u32 size, used, seq, changes, dirty_ms;
    u32 first_ms;                           /* when the oldest change not saved yet was made (valid while dirty) */
    u32 fail_ms, retry_ms;                  /* failing: when the last try failed, the pause before the next (0 - none) */
    u32 flen;                               /* the fixed length of a slot file */
    int dirty, failing;
    int in_place[2];                        /* by slot (seq & 1): the slot file is there with flen bytes, rewritten in place */
    volatile int busy;                      /* the store mutex, see enter() */
};

static omk_store stores[STORE_MAX];
static u32 nstores;
static int card = 1;                        /* 0 - changes stay in memory (store_card, the debug service) */

void store_card(int on)
{
    card = on;
}

/* Store mutex. Callers are tasks on CPU 0 (modules, the core task, the card-off gate), never interrupts. The firmware lock
 * (interrupts off) guards only the flag; the work - copies up to the store size, file writes - runs with interrupts on.
 * A task that finds the store busy sleeps 1 ms and tries again: the holder is a task too and finishes. No RTOS semaphore
 * has been found yet; this is the usual stand-in */
static void enter(omk_store *s)
{
    u32 st;

    for (;;) {
        st = drv_lock();
        if (!s->busy) {
            s->busy = 1;
            drv_unlock(st);
            return;
        }
        drv_unlock(st);
        drv_sleep(1);
    }
}

static void leave(omk_store *s)
{
    s->busy = 0;
}

static u32 align4(u32 n)
{
    return (n + 3) & ~(u32)3;
}

static u32 rec_key(const u8 *r)
{
    return *(const u16 *)r;
}

static u32 rec_len(const u8 *r)
{
    return *(const u16 *)(r + 4);
}

static u32 rec_size(const u8 *r)
{
    return REC_HEAD + align4(rec_len(r));
}

static u32 crc32(const u8 *p, u32 n)
{
    return crc32_update(0, p, n);
}

static u8 *find(omk_store *s, u32 key)
{
    u8 *r;

    for (r = s->mem; r < s->mem + s->used; r += rec_size(r))
        if (rec_key(r) == key)
            return r;
    return 0;
}

static void remove_rec(omk_store *s, u8 *r)
{
    u32 n = rec_size(r);

    mem_copy(r, r + n, (u32)(s->mem + s->used - (r + n)));
    s->used -= n;
}

static int same(const u8 *a, const u8 *b, u32 n)
{
    while (n--)
        if (*a++ != *b++)
            return 0;
    return 1;
}

/* NAME.STA or NAME.STB in the OMKit folder. Three-letter extension: otherwise the firmware FAT cuts the name (emulator: CORE.SA -> COR.ESA) */
static void make_path(char *path, const omk_store *s, u32 seq)
{
    char name[13];

    omk_name_ext(name, s->name, seq & 1 ? "STA" : "STB");   /* the name was checked at store_open */
    omk_path(path, STORE_DIR, name);
}

int store_set(omk_store *s, u16 key, const void *data, u32 len, u32 flags)
{
    u32 now = omk_ms(), need = REC_HEAD + align4(len), room, old_flags = 0;
    u8 *r;
    int changed = 1;

    if (!s || len > 0xffff)
        return -1;
    enter(s);
    r = find(s, key);
    if (r && rec_len(r) == len) {
        old_flags = r[2];
        changed = old_flags != flags || !same(r + REC_HEAD, data, len);
        mem_copy(r + REC_HEAD, data, len);
        r[2] = flags;
    } else {
        room = s->size - s->used + (r ? rec_size(r) : 0);
        if (need > room) {
            leave(s);
            return -1;
        }
        if (r) {
            old_flags = r[2];
            remove_rec(s, r);
        }
        r = s->mem + s->used;
        *(u16 *)r = key;
        r[2] = flags;
        r[3] = 0;
        *(u16 *)(r + 4) = len;
        *(u16 *)(r + 6) = 0;
        mem_copy(r + REC_HEAD, data, len);
        s->used += need;
    }
    if (changed && ((flags | old_flags) & OMK_STORE_PERSIST)) {
        if (!s->dirty)
            s->first_ms = now;
        s->dirty = 1;
        s->dirty_ms = now;
        s->changes++;
    }
    leave(s);
    return 0;
}

int store_get(omk_store *s, u16 key, void *buf, u32 max)
{
    u32 n;
    u8 *r;

    if (!s)
        return -1;
    enter(s);
    r = find(s, key);
    if (!r) {
        leave(s);
        return -1;
    }
    n = rec_len(r);
    mem_copy(buf, r + REC_HEAD, n < max ? n : max);
    leave(s);
    return n;
}

int store_del(omk_store *s, u16 key)
{
    u32 now = omk_ms();
    u8 *r;

    if (!s)
        return -1;
    enter(s);
    r = find(s, key);
    if (!r) {
        leave(s);
        return -1;
    }
    if (r[2] & OMK_STORE_PERSIST) {
        if (!s->dirty)
            s->first_ms = now;
        s->dirty = 1;
        s->dirty_ms = now;
        s->changes++;
    }
    remove_rec(s, r);
    leave(s);
    return 0;
}

/* A save did not reach the card: the next try of store_service after a pause that doubles up to the limit */
static void save_failed(omk_store *s)
{
    s->failing = 1;
    s->fail_ms = omk_ms();
    s->retry_ms = !s->retry_ms ? 2 * STORE_DELAY_MS : s->retry_ms >= STORE_RETRY_MAX_MS / 2 ? STORE_RETRY_MAX_MS :
                  2 * s->retry_ms;
}

/* A/B slots: the new version goes to the other slot file, the current one stays intact until it is written */
static void save(omk_store *s)
{
    u32 changes, out = 0, count = 0, seq = s->seq + 1, w;
    int made, rc;
    u32 *head = (u32 *)s->image;
    char path[OMK_PATH_MAX];
    u8 *r;
    void *fp;

    if (!card)
        return;
    enter(s);
    for (r = s->mem; r < s->mem + s->used; r += rec_size(r))
        if (r[2] & OMK_STORE_PERSIST) {
            mem_copy(s->image + FILE_HEAD + out, r, rec_size(r));
            out += rec_size(r);
            count++;
        }
    changes = s->changes;
    leave(s);
    head[0] = STORE_MAGIC;
    head[1] = STORE_VERSION | count << 16;
    head[2] = seq;
    head[3] = out;
    head[4] = crc32(s->image + FILE_HEAD, out);
    mem_set(s->image + FILE_HEAD + out, 0, s->flen - FILE_HEAD - out);
    make_path(path, s, seq);
    core_card_write(1, path);
    made = !s->in_place[seq & 1];
    if (!made) {
        fp = drv_fopen(path, "r+b");        /* no truncation: the same length in place */
    } else {
        omk_dir(STORE_DIR);                 /* the card may have been formatted since boot */
        drv_remove(path);                   /* "w" does not truncate here: an old file of another length would keep it */
        fp = drv_fopen(path, "wb");
    }
    if (!fp) {                              /* no card, or the card's file module refuses now (the camera's PC mode):
                                               the file is as it was - in place still holds for the next try */
        core_card_write(0, path);
        if (!s->failing)                    /* once: without a card it fails on every try */
            log_printf(OMK_LOG_WARN, "store %s: cannot open %s", s->name, path);
        save_failed(s);
        return;
    }
    w = drv_fwrite(s->image, 1, s->flen, fp);
    rc = drv_fclose(fp);                    /* fclose writes the directory entry (fflush inside) */
    core_card_write(0, path);
    if (rc) {                               /* the firmware's file module refused: the save is tried again later */
        if (!s->failing)
            log_printf(OMK_LOG_WARN, "store %s: closing %s failed, code %x", s->name, path, rc);
        save_failed(s);
        return;
    }
    if (made)                               /* a file made short is made again next time; a rewrite in place keeps the
                                               file's length whatever it took */
        s->in_place[seq & 1] = w == s->flen;
    if (made && w == s->flen)
        log_printf(OMK_LOG_INFO, "store %s: %s made, %u bytes, rewritten in place from now on", s->name, path, s->flen);
    if (w != s->flen) {                     /* card full or failing: the other slot still has the last good version */
        if (!s->failing)
            log_printf(OMK_LOG_WARN, "store %s: %s took %u of %u bytes", s->name, path, w, s->flen);
        save_failed(s);
        return;
    }
    if (s->failing)
        log_printf(OMK_LOG_INFO, "store %s: card back, saved", s->name);
    s->failing = 0;
    s->retry_ms = 0;
    s->seq = seq;
    enter(s);
    if (s->changes == changes)
        s->dirty = 0;
    else
        s->first_ms = omk_ms();             /* changed while it was being written: those changes are the oldest now */
    leave(s);
    log_printf(OMK_LOG_DEBUG, "store %s: saved version %u, %u keys, %u bytes", s->name, seq, count, out);
}

/* Version from a slot file: the version number if the file is intact, otherwise 0. The image stays in s->image. Also
 * whether the file can be rewritten in place: it is there with exactly flen bytes (intact or not) */
static u32 read_slot(omk_store *s, u32 slot)
{
    u32 *head = (u32 *)s->image, got, bytes;
    char path[OMK_PATH_MAX];
    u8 *r, more;
    void *fp;

    make_path(path, s, slot);
    s->in_place[slot & 1] = 0;
    fp = drv_fopen(path, "r");
    if (!fp)
        return 0;
    got = drv_fread(s->image, 1, s->flen, fp);
    s->in_place[slot & 1] = got == s->flen && drv_fread(&more, 1, 1, fp) == 0;
    drv_fclose(fp);
    if (got < FILE_HEAD || head[0] != STORE_MAGIC || (head[1] & 0xffff) != STORE_VERSION)
        return 0;
    bytes = head[3];
    if (bytes > got - FILE_HEAD || bytes > s->size || (head[2] & 1) != (slot & 1) ||
        crc32(s->image + FILE_HEAD, bytes) != head[4])
        return 0;
    for (r = s->image + FILE_HEAD; r < s->image + FILE_HEAD + bytes; r += rec_size(r))
        if (r + REC_HEAD > s->image + FILE_HEAD + bytes)
            return 0;
    if (r != s->image + FILE_HEAD + bytes)
        return 0;
    return head[2];
}

/* The intact image read_slot left in s->image becomes the store's records */
static void take_image(omk_store *s)
{
    s->used = ((u32 *)s->image)[3];
    mem_copy(s->mem, s->image + FILE_HEAD, s->used);
}

/* The newer of the two slot files. Each file is opened and read once: slot A's records are taken before slot B is read
 * into the same image buffer, and B's replace them only when B is intact and newer (an open is the slow part of the
 * card at power-on; reading the better slot a second time cost one more per store) */
static void load(omk_store *s)
{
    u32 a = read_slot(s, 1), b, best;

    if (a)
        take_image(s);
    b = read_slot(s, 2);
    if (b > a)
        take_image(s);
    best = a > b ? a : b;
    if (!best) {
        log_printf(OMK_LOG_INFO, "store %s: no saved data", s->name);
        return;
    }
    s->seq = best;
    log_printf(OMK_LOG_INFO, "store %s: loaded version %u, %u bytes%s", s->name, best, s->used,
               a && b ? "" : ", one copy");
}

omk_store *store_open(const char *name, u32 size)
{
    return store_open_for(name, size, MEM_OWNER_CORE);
}

omk_store *store_open_for(const char *name, u32 size, int owner)
{
    omk_store *s;
    u32 i, k;

    for (i = 0; name[i]; i++)
        if (i == NAME_MAX || !((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '_'))
            return 0;
    if (!i)
        return 0;
    for (k = 0; k < nstores && stores[k].mem; k++)     /* a closed slot is reused */
        ;
    if (k == STORE_MAX)
        return 0;
    s = &stores[k];
    size = align4(size);
    s->mem = mem_alloc_for(owner, size, MEM_IMAGE);
    s->flen = (FILE_HEAD + size + SECTOR - 1) & ~(u32)(SECTOR - 1);
    s->image = mem_alloc_for(owner, s->flen, MEM_IMAGE);
    if (!s->mem || !s->image) {
        mem_free(s->mem);
        mem_free(s->image);
        s->mem = s->image = 0;
        return 0;
    }
    mem_copy(s->name, name, i + 1);
    s->size = size;
    s->used = s->seq = s->changes = 0;      /* the slot of a closed store (a module that failed to start) keeps nothing */
    s->dirty = s->failing = 0;
    s->in_place[0] = s->in_place[1] = 0;
    s->retry_ms = 0;
    if (k == nstores)
        nstores++;
    load(s);
    return s;
}

/* Changes are not written, the owner frees the memory. The handle is no longer valid */
void store_close(omk_store *s)
{
    enter(s);
    s->dirty = 0;
    s->mem = s->image = 0;
    s->name[0] = 0;
    leave(s);
}

void store_service(u32 now)
{
    u32 i;

    if (!card)
        return;
    for (i = 0; i < nstores; i++)
        if (stores[i].mem && stores[i].dirty &&
            (now - stores[i].dirty_ms >= STORE_DELAY_MS || now - stores[i].first_ms >= STORE_LATEST_MS) &&
            (!stores[i].retry_ms || now - stores[i].fail_ms >= stores[i].retry_ms))
            save(&stores[i]);
}

/* The card may be back (power.c: running again): failing saves are tried at the next store_service */
void store_retry_now(void)
{
    u32 i;

    for (i = 0; i < nstores; i++)
        stores[i].retry_ms = 0;
}

/* Writes one store now if it has changes (core task only): the boot record, before anything that may crash. Not an extra
 * write: the change would be saved STORE_DELAY_MS later anyway */
void store_save_now(omk_store *s)
{
    if (s && s->mem && s->dirty)
        save(s);
}

void store_save_all(void)
{
    u32 i;

    for (i = 0; i < nstores; i++)
        if (stores[i].mem && stores[i].dirty)
            save(&stores[i]);
}

void store_info(omk_store *s, u32 *keys, u32 *used, u32 *seq)
{
    u32 n = 0;
    u8 *r;

    enter(s);
    for (r = s->mem; r < s->mem + s->used; r += rec_size(r))
        n++;
    *keys = n;
    *used = s->used;
    *seq = s->seq;
    leave(s);
}

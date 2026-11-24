/* Log: a ring buffer between the tasks that log (any task on CPU 0) and the core service loop, which appends what is new to
 * the files on the card. A line is formatted on the caller's stack outside the firmware lock; under the lock it is only
 * copied into the ring. A line that does not fit is dropped whole and counted.
 *
 * No card or no folder (the file does not open): the service loop tries again with a growing pause - LOG_RETRY_MIN_MS,
 * doubled after every failure up to LOG_RETRY_MAX_MS - instead of at every loop; a write that reaches the card ends the
 * pauses, and so does log_retry_now (the camera is running again after a power-off that did not happen or a sleep: the
 * card may be back). A direct log_flush (the card-off gate, a checkpoint) always tries. Meanwhile the lines wait in the
 * ring; when it is full, new ones are dropped and counted as always.
 *
 * One file, LOG\CORE.LOG, appended to across power-ons: it is never removed or cut by core (each start writes a header
 * line, log_banner, so runs are told apart). Removing and making the file at every start, as core did before, changed the
 * card's allocation bitmap, FAT and directory entry set at every start, and a reset between those writes could break the
 * volume; an append changes the length in the entry set only, and the FAT and the bitmap once a cluster.
 *
 * The file stays open while the log is being written: every portion is fwrite and fflush (the data, the length in the
 * directory entry and, for a new cluster, the FAT and the bitmap reach the card at once, as an fclose would do). An
 * fopen in "ab" and the first write after it walk the file's FAT chain from its first cluster (the firmware keeps the
 * current cluster in the open file only, 0x606ba8df / 0x606bae1c): a cost that grows with the file - in memory while the
 * chain's FAT windows are in the firmware's cache of 8, 32 sectors read per window beyond that. With the file open a
 * portion costs the same at any length. It is closed after LOG_HOLD_MS without anything to write, when the camera leaves
 * the running state (sleep, power-off: log_close from power.c), before the debug service's file jobs, and on an error. */
#include "omk_core.h"
#include "omk_log.h"

#define LOG_NAME "CORE.LOG"
#define RING_SIZE 32768                     /* a power of two */
#define RING_MASK (RING_SIZE - 1)
#define LINE_MAX 192                        /* longer lines are cut, '~' at the end */
#define LOG_QUIET_MS 500                    /* write when the log goes quiet ... */
#define LOG_MAX_WAIT_MS 3000                /* ... but at least every 3 s during a continuous stream */
#define LOG_RETRY_MIN_MS 1000               /* the pause after the first append that did not reach the card ... */
#define LOG_RETRY_MAX_MS 60000              /* ... and the longest one: a card put back is noticed within a minute */
#define LOG_HOLD_MS 30000                   /* the open file is closed after this long without a portion to write */
#define LOG_FAILS_MAX 8                     /* portions a file opened anew does not take, in a row, before it is given up */

typedef struct {
    const char *path;
    u32 done;                               /* ring position up to which the file has the text */
    int state;                              /* FILE_NEW (its folder not checked yet this run), FILE_OPEN or FILE_STOPPED */
} log_file;

enum { FILE_NEW, FILE_OPEN, FILE_STOPPED };

typedef struct {                            /* a line being formatted */
    char *p;
    u32 n;
    int cut;
} line_buf;

static char ring[RING_SIZE];
static u32 head, tail, lost, lost_told, last_change, last_flush, dropped, dropped_told;
static u32 retry_ms, retries;               /* the pause before log_service tries again (0 - the last append was taken);
                                               failed tries in a row */
static int dirty, enabled = 1, level = OMK_LOG_INFO;
static int card = 1;                        /* 0 - nothing goes to the card (log_card); the tee may still keep lines */
static log_tee_fn tee;                      /* the debug service's RAM ring: lines up to tee_level, under the lock */
static int tee_level = OMK_LOG_OFF;
static char log_path[OMK_PATH_MAX];
static log_file core_file;
static void *log_fp;                        /* the open log file, or 0 */
static u32 log_fid;                         /* its identity at the open (drv_file_id) */

static void check_open(const char *path);
static u32 fails;                           /* portions a file opened anew did not take, in a row */
static u32 last_write;                      /* when the last portion was written to it */

void log_setup(int on, int lvl)
{
    enabled = on;
    level = lvl;
}

int log_level(void)
{
    int card_level = enabled && card ? level : OMK_LOG_OFF;

    return tee && tee_level > card_level ? tee_level : card_level;
}

void log_tee(log_tee_fn fn, int lvl)
{
    tee = fn;
    tee_level = lvl;
}

void log_card(int on)
{
    card = on;
}

static void put(line_buf *b, char c)
{
    if (b->n < LINE_MAX - 3)                /* room for '~' and the line end */
        b->p[b->n++] = c;
    else
        b->cut = 1;
}

static void put_str(line_buf *b, const char *s)
{
    while (*s)
        put(b, *s++);
}

static void put_num(line_buf *b, u32 v, u32 base, int width, int zero, int upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[10];
    int n = 0;

    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    while (width-- > n)
        put(b, zero ? '0' : ' ');
    while (n)
        put(b, tmp[--n]);
}

void log_vprintf_from(int lvl, const char *who, const char *fmt, va_list ap)
{
    char text[LINE_MAX];
    line_buf b = {text, 0, 0};
    u32 now, st, i, n;
    int width, zero, to_card = enabled && card && lvl <= level, to_tee = tee && lvl <= tee_level;

    if (lvl <= OMK_LOG_OFF || (!to_card && !to_tee))
        return;
    now = omk_ms();
    put_num(&b, now, 10, 8, 0, 0);
    put(&b, ' ');
    put(&b, "?EWID"[lvl]);
    put(&b, ' ');
    if (who) {
        put_str(&b, who);
        put(&b, ':');
        put(&b, ' ');
    }
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&b, *fmt);
            continue;
        }
        fmt++;
        zero = *fmt == '0';
        for (width = 0; *fmt >= '0' && *fmt <= '9'; fmt++)
            width = width * 10 + *fmt - '0';
        switch (*fmt) {
        case 's':
            put_str(&b, va_arg(ap, const char *));
            break;
        case 'c':
            put(&b, (char)va_arg(ap, int));
            break;
        case 'd': {
            int v = va_arg(ap, int);

            if (v < 0) {
                put(&b, '-');
                put_num(&b, -(u32)v, 10, width ? width - 1 : 0, zero, 0);
            } else {
                put_num(&b, v, 10, width, zero, 0);
            }
            break;
        }
        case 'u':
            put_num(&b, va_arg(ap, u32), 10, width, zero, 0);
            break;
        case 'x':
        case 'X':
            put_num(&b, va_arg(ap, u32), 16, width, zero, *fmt == 'X');
            break;
        case 0:
            fmt--;
            break;
        default:
            put(&b, *fmt);
        }
    }
    if (b.cut)
        text[b.n++] = '~';
    text[b.n++] = '\r';
    text[b.n++] = '\n';
    n = b.n;
    st = drv_lock();
    if (to_tee)
        tee(lvl, now, text, n - 2);         /* without the line end */
    if (!to_card) {
    } else if (head - tail + n > RING_SIZE) {   /* the card has not taken the older lines yet: drop this one whole */
        lost++;
    } else {
        for (i = 0; i < n; i++)
            ring[(head + i) & RING_MASK] = text[i];
        head += n;
        dirty = 1;
        last_change = now;
    }
    drv_unlock(st);
}

void log_vprintf(int lvl, const char *fmt, va_list ap)
{
    log_vprintf_from(lvl, 0, fmt, ap);
}

void log_printf(int lvl, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_vprintf(lvl, fmt, ap);
    va_end(ap);
}

void log_checkpoint(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_vprintf(OMK_LOG_DEBUG, fmt, ap);
    va_end(ap);
    if (level >= OMK_LOG_DEBUG)
        log_flush();
}

/* ring[from, from + n) to the file, at most two pieces; returns what fwrite took */
static u32 write_ring(void *fp, u32 from, u32 n)
{
    u32 at = from & RING_MASK, first = n < RING_SIZE - at ? n : RING_SIZE - at, w;

    w = drv_fwrite(ring + at, 1, first, fp);
    if (w == first && n > first)
        w += drv_fwrite(ring, 1, n - first, fp);
    return w;
}

/* An empty line, then the line: the header of a run in a log that holds many */
void log_banner(const char *fmt, ...)
{
    static const char gap[2] = {'\r', '\n'};
    va_list ap;
    u32 st, i;

    if (enabled && card && OMK_LOG_INFO <= level) {
        st = drv_lock();
        if (head - tail + sizeof(gap) <= RING_SIZE) {
            for (i = 0; i < sizeof(gap); i++)
                ring[(head + i) & RING_MASK] = gap[i];
            head += sizeof(gap);
            dirty = 1;
        }
        drv_unlock(st);
    }
    va_start(ap, fmt);
    log_vprintf(OMK_LOG_INFO, fmt, ap);
    va_end(ap);
}

/* Appends ring[f->done, h) to the file. 0 - the card does not take it now: try again later (log_flush's pause).
 *
 * The open file may stop working while it is open: the camera's PC mode (USB, MTP) gave every call on it the firmware's
 * error 9414 (0x24C6: fwrite took 0 bytes, fflush and fclose returned it - camera, card157), while a file opened anew
 * worked. So a portion the open file does not take closes it and goes once more to a file opened anew; only a file
 * opened anew that does not take it is a failure of the card: the text stays in the ring and is tried again after the
 * pause, and after LOG_FAILS_MAX such failures in a row (a full or failing card) the file is given up for this run -
 * the text from then on is counted as lost. A portion that fwrite took but fflush did not is written again (its text
 * twice rather than lost) */
static int append(log_file *f, u32 h)
{
    u32 n, w, rc, pass;
    int fresh;

    if (f->state == FILE_STOPPED || h == f->done)
        return 1;
    if (!drv_card_mounted())                /* the card's volume is not mounted now (USB Storage): later */
        return 0;
    core_card_write(1, f->path);
    for (pass = 0; pass < 2; pass++) {
        n = h - f->done;
        check_open(f->path);
        fresh = !log_fp;
        if (fresh) {
            if (f->state == FILE_NEW)
                omk_dir(LOG_DIR);           /* the card may have been formatted since boot */
            log_fp = drv_fopen(f->path, "ab");  /* opens or creates, at the end */
            if (!log_fp) {
                core_card_write(0, f->path);
                if (!retries)               /* the first of a row of failures: the reason for the debug service's RAM
                                               log (the card's file only gets it when it works again) */
                    log_printf(OMK_LOG_WARN, "log: cannot open %s: no card, or the card's file module refuses", f->path);
                return 0;
            }
            log_fid = drv_file_id(log_fp);
            f->state = FILE_OPEN;
        }
        w = write_ring(log_fp, f->done, n);
        rc = drv_fflush(log_fp);            /* the length and the clusters to the card now, as fclose would */
        if (!rc)
            f->done += w;
        last_write = omk_ms();
        if (w == n && !rc) {
            core_card_write(0, f->path);
            fails = 0;
            return 1;
        }
        log_close();
        log_printf(OMK_LOG_WARN, "log: %s took %u of %u bytes, fflush %x (%s file): %s", f->path, w, n, rc,
                   fresh ? "a new" : "the open", fresh ? "tried again later" : "opened again");
        if (fresh)
            break;
    }
    core_card_write(0, f->path);
    if (++fails >= LOG_FAILS_MAX) {         /* a full or failing card: not tried for ever */
        f->state = FILE_STOPPED;
        log_printf(OMK_LOG_ERROR, "log: %s failed %u times in a row, no more writes to it in this run", f->path, fails);
        return 1;
    }
    return 0;
}

/* Service loop (and the card-off gate) only: file calls */
void log_flush(void)
{
    u32 st, h, free_to;
    int ok;

    if (!enabled || !card)
        return;
    st = drv_lock();
    h = head;
    dirty = 0;
    drv_unlock(st);
    if (!log_path[0])
        omk_path(log_path, LOG_DIR, LOG_NAME);
    core_file.path = log_path;
    ok = append(&core_file, h);
    free_to = core_file.done;               /* the ring is free up to what the file has */
    if (core_file.state == FILE_STOPPED) {  /* given up: the text not written is dropped */
        if (h != core_file.done)
            dropped += h - core_file.done;
        core_file.done = free_to = h;
    }
    st = drv_lock();
    tail = free_to;
    if (!ok)
        dirty = 1;                          /* no card (sleep, power-off cancelled after unmount): try again later */
    drv_unlock(st);
    last_flush = omk_ms();
    if (!ok) {                              /* ... after a pause that doubles up to the limit */
        retry_ms = !retry_ms ? LOG_RETRY_MIN_MS : retry_ms >= LOG_RETRY_MAX_MS / 2 ? LOG_RETRY_MAX_MS : retry_ms * 2;
        retries++;
    } else {
        if (retries)
            log_printf(OMK_LOG_INFO, "log: the card takes the log again, after %u tries without it", retries);
        retry_ms = retries = 0;
    }
    if (lost != lost_told) {
        log_printf(OMK_LOG_WARN, "log: %u lines lost, the buffer was full", lost - lost_told);
        lost_told = lost;
    }
    if (dropped != dropped_told) {          /* for the debug service's RAM log: the card's file is given up */
        log_printf(OMK_LOG_WARN, "log: %u bytes not written, the file was given up", dropped - dropped_told);
        dropped_told = dropped;
    }
}

/* Closes the open log file; the next portion opens it again. Core task or the card-off gate (power.c) */
void log_close(void)
{
    void *fp = log_fp;
    int rc;

    if (!fp)
        return;
    log_fp = 0;
    if (drv_file_id(fp) != log_fid)         /* the firmware freed it (a mount): not ours to close - the slot may be
                                               another file's now */
        return;
    rc = drv_fclose(fp);
    if (rc)
        log_printf(OMK_LOG_WARN, "log: closing %s failed, code %x", log_path, rc);
}

/* The open file freed by the firmware (the card mounted again - the camera's PC mode): forgotten without fclose and
 * opened again; a write through it could go to another file that got its slot */
static void check_open(const char *path)
{
    if (log_fp && drv_file_id(log_fp) != log_fid) {
        log_fp = 0;
        log_printf(OMK_LOG_INFO, "log: %s was closed by the firmware (the card was mounted again), opened again", path);
    }
}

/* Free bytes in the ring now: a writer of many lines (the task dump) waits while it is low, rather than losing lines */
u32 log_room(void)
{
    u32 st = drv_lock(), used = head - tail;

    drv_unlock(st);
    return RING_SIZE - used;
}

void log_service(u32 now)
{
    if (log_fp && !dirty && now - last_write >= LOG_HOLD_MS)
        log_close();
    if (!dirty || (retry_ms && now - last_flush < retry_ms))   /* nothing new, or the card did not take the last try */
        return;
    if (now - last_change >= LOG_QUIET_MS || now - last_flush >= LOG_MAX_WAIT_MS)
        log_flush();
}

/* The card may be back (power.c: running again): the next log_service tries at once */
void log_retry_now(void)
{
    retry_ms = 0;
}

/* Log: a ring buffer between the tasks that log (any task on CPU 0) and the core service loop, which appends what is new to
 * the files on the card. A line is formatted on the caller's stack outside the firmware lock; under the lock it is only
 * copied into the ring. A line that does not fit is dropped whole and counted.
 *
 * No card or no folder (the file does not open): the service loop tries again with a growing pause - LOG_RETRY_MIN_MS,
 * doubled after every failure up to LOG_RETRY_MAX_MS - instead of at every loop; a write that reaches the card ends the
 * pauses, and so does log_retry_now (the camera is running again after a power-off that did not happen or a sleep: the
 * card may be back). A direct log_flush (the card-off gate, a checkpoint) always tries. Meanwhile the lines wait in the
 * ring; when it is full, new ones are dropped and counted as always. */
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
#define LOG_KEEP_MAX (1024 * 1024)          /* a kept log (log_keep_previous) longer than this starts empty after all */

typedef struct {
    const char *path;
    u32 done;                               /* ring position up to which the file has the text */
    int state;                              /* FILE_NEW, FILE_OPEN or FILE_STOPPED */
    int keep;                               /* FILE_NEW: append to the old text instead of removing the file */
} log_file;

enum { FILE_NEW, FILE_OPEN, FILE_STOPPED };

typedef struct {                            /* a line being formatted */
    char *p;
    u32 n;
    int cut;
} line_buf;

static char ring[RING_SIZE];
static u32 head, tail, lost, lost_told, last_change, last_flush;
static u32 retry_ms, retries;               /* the pause before log_service tries again (0 - the last append was taken);
                                               failed tries in a row */
static int dirty, enabled = 1, level = OMK_LOG_INFO;
static char log_path[OMK_PATH_MAX];
static log_file core_file, run_file;

void log_setup(int on, int lvl, const char *path)
{
    enabled = on;
    level = lvl;
    if (path != run_file.path) {            /* a run log gets what is logged from now on */
        run_file.path = path;
        run_file.done = head;
        run_file.state = FILE_NEW;
    }
}

int log_level(void)
{
    return enabled ? level : OMK_LOG_OFF;
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
    int width, zero;

    if (!enabled || lvl <= OMK_LOG_OFF || lvl > level)
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
    if (head - tail + n > RING_SIZE) {      /* the card has not taken the older lines yet: drop this one whole */
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

void log_keep_previous(void)
{
    if (core_file.state == FILE_NEW)
        core_file.keep = 1;
}

/* Appends ring[f->done, h) to the file. 0 - the card is not there now: try again later */
static int append(log_file *f, u32 h)
{
    u32 n = h - f->done, w, kept = 0;
    void *fp;

    if (f->state == FILE_STOPPED || !n)
        return 1;
    if (f->state == FILE_NEW) {
        omk_dir(LOG_DIR);                   /* the card may have been formatted since boot */
        if (!f->keep)
            drv_remove(f->path);            /* a new power-on: the file starts empty ("w" does not truncate here) */
    }
    fp = drv_fopen(f->path, "ab");          /* opens or creates, at the end */
    if (!fp)
        return 0;
    w = write_ring(fp, f->done, n);
    if (f->state == FILE_NEW && f->keep && w == n) {
        kept = drv_ftell(fp);               /* after a write: the file's length */
        if (kept > LOG_KEEP_MAX) {          /* a crash loop must not grow it without end: this run starts it again */
            drv_fclose(fp);
            drv_remove(f->path);
            fp = drv_fopen(f->path, "ab");
            if (!fp)
                return 0;
            w = write_ring(fp, f->done, n);
        }
    }
    f->state = FILE_OPEN;
    drv_fclose(fp);                         /* no fflush: suspected of blocking power-off */
    f->done += w;
    if (w < n) {                            /* card full or failing: stop rather than retry forever */
        f->state = FILE_STOPPED;
        f->done = h;
        log_printf(OMK_LOG_ERROR, "log: %s took %u of %u bytes, no more writes to it", f->path, w, n);
    }
    if (kept)
        log_printf(kept > LOG_KEEP_MAX ? OMK_LOG_WARN : OMK_LOG_INFO, kept > LOG_KEEP_MAX ?
                   "log: the previous run's log in %s was over %u bytes, started again" :
                   "log: %s keeps the previous run's log, %u bytes now", f->path, kept > LOG_KEEP_MAX ? LOG_KEEP_MAX : kept);
    return 1;
}

/* Service loop (and the card-off gate) only: file calls. 1 - the files took everything up to now (a file stopped for a
 * full card counts as not) */
int log_flush(void)
{
    u32 st, h, free_to;
    int ok;

    if (!enabled)
        return 0;
    st = drv_lock();
    h = head;
    dirty = 0;
    drv_unlock(st);
    if (!log_path[0])
        omk_path(log_path, LOG_DIR, LOG_NAME);
    core_file.path = log_path;
    ok = append(&core_file, h);
    if (run_file.path)
        ok &= append(&run_file, h);
    free_to = h;                            /* the ring is free up to what every file still being written has */
    if (core_file.state != FILE_STOPPED && h - core_file.done > h - free_to)
        free_to = core_file.done;
    if (run_file.path && run_file.state != FILE_STOPPED && h - run_file.done > h - free_to)
        free_to = run_file.done;
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
    return ok && core_file.state != FILE_STOPPED && core_file.done == h &&
           (!run_file.path || (run_file.state != FILE_STOPPED && run_file.done == h));
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

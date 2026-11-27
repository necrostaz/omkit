/* Memory manager: first fit in the driver regions, neighbours are merged on free.
 * Block: 16-byte header {size including header, size of the previous block, owner, flags, tag}, then data.
 * A region ends with a stub header. Blocks from the firmware heap have the same header and the FW flag.
 * The driver lock only works on its own CPU, so calls from CPUs 1 and 2 are rejected */
#include "omk_core.h"
#include "omk_log.h"
#include "omk_mem.h"

#define HEAD 16
#define ALIGN 16
#define SPLIT_MIN 64
#define LINE MEM_LINE                       /* MEM_LINES: the block's data owns whole lines of this size */
#define REGIONS 4
#define MAGIC 0x424b4d4f                    /* 'OMKB' */
#define F_USED 1
#define F_END 2
#define F_FW 4
#define F_CLASS_SHIFT 8                     /* firmware heap class of an FW block */
#define FW_CLASSES 4
#define FW_RESERVE_DIV 4                    /* leave at least a quarter of the class to the firmware */
#define NAME_MAX 8
#define FAIL_REPORT_MS 10000
#define SUMMARY_MS 30000
#define TICK_MS 1000
#define FULL_PCT 90

typedef struct {
    u32 size, prev;
    u16 owner, flags;
    u32 magic;
} blk;

typedef struct {
    char name[NAME_MAX + 1];
    omk_mem_info i;
    u32 fail_last, fail_new, fail_ms;
    int fail_seen;
} owner_t;

typedef struct {
    u32 block, total, free, min_free;
    int low;
    u32 ours;                               /* our blocks in the class */
} fw_class_t;

static struct {
    u8 *base;
    u32 size;
} regions[REGIONS];
static u32 nregions, arena_total, arena_used, warned_pct, next_tick, next_summary;
static owner_t owners[MEM_OWNERS];
static u32 nowners;
static int fw_on;
static u32 fw_limit, fw_used, nclasses;
static fw_class_t fw[FW_CLASSES];

static u32 align(u32 n)
{
    return (n + ALIGN - 1) & ~(u32)(ALIGN - 1);
}

static blk *next_blk(blk *b)
{
    return (blk *)((u8 *)b + b->size);
}

static int valid(const blk *b)
{
    return b->magic == MAGIC && !(b->size & (ALIGN - 1)) && b->owner < nowners;
}

void mem_init(void)
{
    void *base;
    u32 size, i;
    blk *b, *end;

    nregions = arena_total = arena_used = warned_pct = next_tick = next_summary = nowners = 0;
    fw_on = 0;
    fw_used = 0;
    mem_owner("CORE", 0);
    for (i = 0; nregions < REGIONS && drv_mem_region(i, &base, &size); i++) {
        u8 *lo = (u8 *)(((uptr)base + ALIGN - 1) & ~(uptr)(ALIGN - 1)), *hi = (u8 *)(((uptr)base + size) & ~(uptr)(ALIGN - 1));

        if (hi - lo < 4 * HEAD)
            continue;
        b = (blk *)lo;
        end = (blk *)(hi - HEAD);
        b->size = (u8 *)end - lo;
        b->prev = 0;
        b->owner = 0;
        b->flags = 0;
        b->magic = MAGIC;
        end->size = 0;
        end->prev = b->size;
        end->owner = 0;
        end->flags = F_USED | F_END;
        end->magic = MAGIC;
        regions[nregions].base = lo;
        regions[nregions++].size = hi - lo;
        arena_total += b->size;
    }
    for (nclasses = 0; nclasses < FW_CLASSES &&
         drv_fwheap_class(nclasses, &fw[nclasses].block, &fw[nclasses].total, &fw[nclasses].free); nclasses++) {
        fw[nclasses].min_free = fw[nclasses].free;
        fw[nclasses].low = 0;
        fw[nclasses].ours = 0;
    }
}

void mem_setup(int fw_heap, u32 limit)
{
    fw_on = fw_heap && limit && nclasses;
    fw_limit = limit;
}

int mem_owner(const char *name, u32 limit)
{
    owner_t *o;
    u32 i;

    if (nowners == MEM_OWNERS)
        return -1;
    o = &owners[nowners];
    mem_set(o, 0, sizeof(*o));
    for (i = 0; name[i] && i < NAME_MAX; i++)
        o->name[i] = name[i];
    o->i.limit = limit;
    return nowners++;
}

const char *mem_owner_name(int owner)
{
    return owner >= 0 && (u32)owner < nowners ? owners[owner].name : "?";
}

/* Under the lock. Largest free block, for the log */
static u32 largest_free(void)
{
    u32 r, best = 0;
    blk *b;

    for (r = 0; r < nregions; r++)
        for (b = (blk *)regions[r].base; !(b->flags & F_END); b = next_blk(b))
            if (!(b->flags & F_USED) && b->size - HEAD > best)
                best = b->size - HEAD;
    return best;
}

/* MEM_LINES: how many bytes to leave free in front of the free block b so that the data of the block made after them
 * starts on a LINE boundary. The piece left in front stays a free block of its own, so it is 0 or at least SPLIT_MIN
 * (blocks are 16-byte aligned: the plain distance is 0, 16, 32 or 48, and LINE more when it is not 0) */
static u32 line_pad(const blk *b)
{
    u32 pad = (u32)(0 - ((uptr)b + HEAD)) & (LINE - 1);

    return pad && pad < SPLIT_MIN ? pad + LINE : pad;
}

/* Under the lock. lines: the data starts on a LINE boundary (need is HEAD + a multiple of LINE then, so the data ends on
 * one too, and whatever follows - the next block's header - starts in a line of its own) */
static blk *take_arena(u32 need, int lines)
{
    u32 r, pad;
    blk *b, *rest, *after;

    for (r = 0; r < nregions; r++)
        for (b = (blk *)regions[r].base; !(b->flags & F_END); b = next_blk(b)) {
            if ((b->flags & F_USED) || b->size < need)
                continue;
            pad = lines ? line_pad(b) : 0;
            if (pad) {
                if (b->size - need < pad)
                    continue;               /* not with the piece in front: the next free block */
                after = next_blk(b);
                rest = (blk *)((u8 *)b + pad);
                rest->size = b->size - pad;
                rest->prev = pad;
                rest->owner = 0;
                rest->flags = 0;
                rest->magic = MAGIC;
                after->prev = rest->size;
                b->size = pad;              /* stays free: merged again when a neighbour is freed */
                b = rest;
            }
            if (b->size - need >= SPLIT_MIN) {
                after = next_blk(b);
                rest = (blk *)((u8 *)b + need);
                rest->size = b->size - need;
                rest->prev = need;
                rest->owner = 0;
                rest->flags = 0;
                rest->magic = MAGIC;
                after->prev = rest->size;
                b->size = need;
            }
            b->flags = F_USED;
            arena_used += b->size;
            return b;
        }
    return 0;
}

/* Firmware heap class for need bytes with a reserve for the firmware, or -1. The free count is current, from the driver */
static int fw_pick(u32 need)
{
    u32 c, block, total, free;

    for (c = 0; c < nclasses; c++)
        if (fw[c].block >= need)
            return drv_fwheap_class(c, &block, &total, &free) && free > total / FW_RESERVE_DIV ? (int)c : -1;
    return -1;
}

static void account(owner_t *o, u32 size)
{
    o->i.used += size;
    o->i.allocs++;
    if (o->i.used > o->i.peak)
        o->i.peak = o->i.used;
}

static void fail(int owner, u32 size, const char *why)
{
    owner_t *o = &owners[owner];
    u32 st = drv_lock(), largest = largest_free();
    int first = !o->fail_seen;

    o->i.fails++;
    o->fail_last = size;
    o->fail_seen = 1;
    if (!first)
        o->fail_new++;
    drv_unlock(st);
    if (first)
        log_printf(OMK_LOG_WARN, "memory: %s wants %u bytes%s; arena free %u, largest %u, firmware heap %u of %u", o->name, size,
                   why, arena_total - arena_used, largest, fw_used, fw_on ? fw_limit : 0);
}

void *mem_alloc_for(int owner, u32 size, u32 flags)
{
    owner_t *o;
    u32 need, st;
    blk *b;
    int c;

    if (owner < 0 || (u32)owner >= nowners)
        return 0;
    o = &owners[owner];
    if (drv_cpu() != 0) {
        fail(owner, size, " from another cpu");
        return 0;
    }
    if (!size || size > 0x10000000) {
        fail(owner, size, ", size out of range");
        return 0;
    }
    /* MEM_LINES (arena only, see omk_mem.h): the size up to whole lines; the place is chosen in take_arena */
    need = (flags & MEM_LINES ? (size + LINE - 1) & ~(u32)(LINE - 1) : align(size)) + HEAD;
    if (o->i.limit && o->i.used + need > o->i.limit) {
        fail(owner, size, ", over its limit");
        return 0;
    }
    st = drv_lock();
    b = take_arena(need, (flags & MEM_LINES) != 0);
    if (b) {
        b->owner = owner;
        account(o, b->size);
        drv_unlock(st);
        return (u8 *)b + HEAD;
    }
    c = !(flags & (MEM_IMAGE | MEM_LINES)) && fw_on && fw_used + need <= fw_limit ? fw_pick(need) : -1;
    drv_unlock(st);
    if (c >= 0 && (b = drv_fwheap_alloc(need)) != 0) {
        st = drv_lock();
        b->size = need;
        b->prev = 0;
        b->owner = owner;
        b->flags = F_USED | F_FW | c << F_CLASS_SHIFT;
        b->magic = MAGIC;
        fw_used += need;
        fw[c].ours++;
        account(o, need);
        o->i.fw_used += need;
        drv_unlock(st);
        return (u8 *)b + HEAD;
    }
    fail(owner, size, "");
    return 0;
}

void *mem_alloc(u32 size)
{
    return mem_alloc_for(MEM_OWNER_CORE, size, MEM_IMAGE);
}

void mem_free(void *p)
{
    blk *b, *n, *pr;
    owner_t *o;
    u32 st;

    if (!p)
        return;
    b = (blk *)((u8 *)p - HEAD);
    if (drv_cpu() != 0) {
        log_printf(OMK_LOG_ERROR, "memory: free %x from another cpu, block kept", (u32)(uptr)p);
        return;
    }
    st = drv_lock();
    if (!valid(b) || (b->flags & (F_USED | F_END)) != F_USED) {
        drv_unlock(st);
        log_printf(OMK_LOG_ERROR, "memory: free %x: block header damaged or not in use, block kept", (u32)(uptr)p);
        return;
    }
    o = &owners[b->owner];
    o->i.used -= b->size;
    if (b->flags & F_FW) {
        u32 c = b->flags >> F_CLASS_SHIFT;

        fw_used -= b->size;
        o->i.fw_used -= b->size;
        fw[c].ours--;
        b->magic = 0;
        drv_unlock(st);
        drv_fwheap_free(b);
        return;
    }
    arena_used -= b->size;
    b->flags = 0;
    b->owner = 0;
    n = next_blk(b);
    if (!(n->flags & F_USED)) {
        b->size += n->size;
        n->magic = 0;
        n = next_blk(b);
        n->prev = b->size;
    }
    if (b->prev) {
        pr = (blk *)((u8 *)b - b->prev);
        if (!(pr->flags & F_USED)) {
            pr->size += b->size;
            b->magic = 0;
            n->prev = pr->size;
        }
    }
    drv_unlock(st);
}

/* Block owner, or -1 if p is not the start of a live block */
int mem_block_owner(void *p)
{
    blk *b = (blk *)((u8 *)p - HEAD);
    u32 st;
    int o = -1;

    if (!p)
        return -1;
    st = drv_lock();
    if (valid(b) && (b->flags & (F_USED | F_END)) == F_USED)
        o = b->owner;
    drv_unlock(st);
    return o;
}

/* All blocks of the owner in the arena except keep. Firmware heap blocks are not in the chain: a warning about them */
void mem_free_owner(int owner, void *keep)
{
    u32 r, st;
    blk *b;
    int again = 1;

    while (again) {
        again = 0;
        st = drv_lock();
        for (r = 0; r < nregions && !again; r++)
            for (b = (blk *)regions[r].base; !(b->flags & F_END); b = next_blk(b))
                if ((b->flags & F_USED) && b->owner == owner && (u8 *)b + HEAD != keep) {
                    again = 1;
                    break;
                }
        drv_unlock(st);
        if (again)
            mem_free((u8 *)b + HEAD);
    }
    if (owner >= 0 && (u32)owner < nowners && owners[owner].i.fw_used)
        log_printf(OMK_LOG_WARN, "memory: %s keeps %u bytes of firmware heap", owners[owner].name, owners[owner].i.fw_used);
}

void mem_info(int owner, omk_mem_info *out)
{
    u32 st = drv_lock();

    if (owner >= 0 && (u32)owner < nowners)
        mem_copy(out, &owners[owner].i, sizeof(*out));
    else
        mem_set(out, 0, sizeof(*out));
    drv_unlock(st);
}

static void mem_usage(u32 *total, u32 *used)
{
    u32 st = drv_lock();

    *total = arena_total;
    *used = arena_used;
    drv_unlock(st);
}

void mem_arena(u32 *total, u32 *used, u32 *largest)
{
    u32 st = drv_lock();

    *total = arena_total;
    *used = arena_used;
    *largest = largest_free();
    drv_unlock(st);
}

static void watch_fw_heap(void)
{
    u32 c, block, total, free;
    int low;

    for (c = 0; c < nclasses; c++) {
        if (!drv_fwheap_class(c, &block, &total, &free))
            continue;
        fw[c].free = free;
        if (free < fw[c].min_free)
            fw[c].min_free = free;
        low = free <= total / FW_RESERVE_DIV;
        if (low && !fw[c].low)
            log_printf(fw[c].ours ? OMK_LOG_WARN : OMK_LOG_INFO, "firmware heap %u: %u of %u blocks free, %u of them ours in use",
                       block, free, total, fw[c].ours);
        else if (!low && fw[c].low)
            log_printf(OMK_LOG_DEBUG, "firmware heap %u: recovered, %u of %u free", block, free, total);
        fw[c].low = low;
    }
}

#ifdef OMK_DEBUG
static void summary(void)
{
    u32 i, total, used, largest;

    mem_arena(&total, &used, &largest);
    log_printf(OMK_LOG_DEBUG, "memory: arena %u of %u used, largest free %u; firmware heap ours %u of %u", used, total, largest,
               fw_used, fw_on ? fw_limit : 0);
    for (i = 0; i < nclasses; i++)
        log_printf(OMK_LOG_DEBUG, "memory: firmware heap %u: %u of %u free, lowest %u, ours %u", fw[i].block, fw[i].free,
                   fw[i].total, fw[i].min_free, fw[i].ours);
    for (i = 0; i < nowners; i++)
        log_printf(OMK_LOG_DEBUG, "memory: %s used %u peak %u allocs %u fails %u firmware heap %u", owners[i].name,
                   owners[i].i.used, owners[i].i.peak, owners[i].i.allocs, owners[i].i.fails, owners[i].i.fw_used);
}
#endif

void mem_service(u32 now)
{
    u32 i, pct, total, used, largest;

    if ((int)(now - next_tick) < 0)
        return;
    next_tick = now + TICK_MS;
    for (i = 0; i < nowners; i++) {
        owner_t *o = &owners[i];

        if (o->fail_new && now - o->fail_ms >= FAIL_REPORT_MS) {
            log_printf(OMK_LOG_WARN, "memory: %s %u more failures, last %u bytes", o->name, o->fail_new, o->fail_last);
            o->fail_new = 0;
            o->fail_ms = now;
        }
    }
    mem_usage(&total, &used);               /* the walk for the largest free block only when it is logged */
    pct = total >= 100 ? used / (total / 100) : 0;
    if (pct >= FULL_PCT && pct >= warned_pct + 2) {
        mem_arena(&total, &used, &largest);
        log_printf(OMK_LOG_WARN, "memory: arena %u%% used, %u bytes free, largest %u", pct, total - used, largest);
        warned_pct = pct;
    }
#ifdef OMK_DEBUG
    watch_fw_heap();
    if (log_level() >= OMK_LOG_DEBUG && (int)(now - next_summary) >= 0) {
        next_summary = now + SUMMARY_MS;
        summary();
    }
#else
    if (fw_on)                              /* the firmware heap's counters only while the modules may take from it */
        watch_fw_heap();
#endif
}

/* Word-wise when source and destination have the same alignment, bytes otherwise; overlapping regions are allowed.
 * may_alias: the words may be parts of any object (GCC's way to stay within the aliasing rules) */
typedef u32 __attribute__((may_alias)) word_t;

static void copy_up(u8 *d, const u8 *s, u32 n)
{
    if (!(((uptr)d ^ (uptr)s) & 3)) {
        for (; ((uptr)d & 3) && n; n--)
            *d++ = *s++;
        for (; n >= 4; n -= 4, d += 4, s += 4)
            *(word_t *)d = *(const word_t *)s;
    }
    while (n--)
        *d++ = *s++;
}

static void copy_down(u8 *d, const u8 *s, u32 n)
{
    d += n;
    s += n;
    if (!(((uptr)d ^ (uptr)s) & 3)) {
        for (; ((uptr)d & 3) && n; n--)
            *--d = *--s;
        for (; n >= 4; n -= 4) {
            d -= 4;
            s -= 4;
            *(word_t *)d = *(const word_t *)s;
        }
    }
    while (n--)
        *--d = *--s;
}

static void fill(u8 *d, int c, u32 n)
{
    u32 w = (u8)c * 0x01010101u;

    for (; ((uptr)d & 3) && n; n--)
        *d++ = c;
    for (; n >= 4; n -= 4, d += 4)
        *(word_t *)d = w;
    while (n--)
        *d++ = c;
}

void mem_copy(void *dst, const void *src, u32 n)
{
    u8 *d = dst;
    const u8 *p = src;

    if (d <= p || d >= p + n)
        copy_up(d, p, n);
    else
        copy_down(d, p, n);
}

void mem_set(void *dst, int c, u32 n)
{
    fill(dst, c, n);
}

/* The look's grain (API 17): the stills of the film look get the grain of the tile and strength its owner chose for the
 * entry shown. core owns the tile containers on the card, loads the chosen tile into one slot in the arena and hands it
 * to the driver (platform graindrv.c), which puts it on the stills through the firmware's own grain film route.
 *
 * Containers OMKIT\GRAIN\NAME.GRN (version 2, OMKit/docs/formats/grn.md): a 64-byte header - u32 'OMGR', u16 version 2,
 * u16 64, u32 the file's size, u32 CRC-32 of the header with this field 0, char[24] the name shown (printable ASCII, zero
 * padded), u16 side (a multiple of 64, 128..768, dividing the firmware's 1536 x 1536 tile), u16 mean x 256, u8 the largest
 * value (<= 63, as in the firmware's own tile), u8 flags 0, u16 the tile's gain (1..1024: the strength High), u16[3] the
 * gains of Low, Med, High (all 0 - 0.25, 0.55 and 1 x the tile's gain; otherwise 1..2047 each, not falling), u16 0, u32
 * CRC-32 of the pixels, u32 0 - then side x side bytes, rows top down.
 *
 * The headers are read when the owner takes the service (grain_list, module_init): the driver's hooks go in then. The
 * pixels of one tile at a time are read into the slot when the choice changes (a pause of LOAD_DELAY_MS first: the dial
 * may still be turning), in CHUNK pieces through a buffer of core (the checks and the CRC run on cached memory), and
 * copied into the slot in the arena, which the hooks read on any CPU. A still taken while the slot does not hold the
 * chosen tile gets no grain (the driver logs it). The image allocator is kept in its large layout only while the look is
 * on with a strength: the change asks the GUI task for a picture mode rebuild (drv_alloc_large).
 *
 * The tone mask (core 1.21, [grain] in OMKIT.INI): mask = film | step | flat | list | off - the route of the look's stills.
 * off: the grain film's work list (the texture blended on the development, one amplitude everywhere); the others: the
 * monochrome profile's list with its mask development (graindrv.c), the texture weighted by a mask whose curve the ISP
 * driver builds from w here - the weight by the picture's Y after the look's tone: flat 1023 everywhere, step 0 below Y
 * 128 and 1023 from it, film the film archive's grain by tone (to the midtones: 0.47 in the shadows below Y 50, 0.90,
 * 1.03, 0.94 and 0.56 in the highlights above Y 200 - linear between the bands' middles, at most 1023); list: the list
 * alone, no blend (the picture without grain; up to core 1.22 no picture at all - graindrv.c, the output step).
 * chroma = 0..1024: the blend generator's noise in Cb and Cr on colour presets (never on a black-and-white one,
 * isp_mono). mask_test = a list of up to 8 of off, list, flat, step, film, chroma (film with chroma_test): a
 * bisection over power-ons - a level a run, mask_jobs of our jobs to pass it; the level under test is written to the
 * card (store GRAIN) before the driver gets it, and a run that did not end with a power-off after it was armed (the
 * allocator in mode 1: a still with grain possible) marks it "reset"; after the list, `mask`. mask_run: another number
 * starts the list again.
 * A release build has mask = film | flat | off and chroma; the test levels step and list, mask_test with its bisection
 * and chroma_test are a debug build's (BIS_ON) */
#include "omk_core.h"
#include "omk_log.h"
#include "omk_mod.h"
#include "omk_isp.h"
#include "omk_power.h"
#include "omk_store.h"

#define TILE_DIR "GRAIN"
#define ARENA_KEEP (768u * 1024)            /* the largest free piece of the arena left after the slot: the modules that
                                               load after the owner need room too */
#define CHUNK 4096
#define NAME 13                             /* 8.3 and the terminator */

static u8 chunk[CHUNK];

#define GRN_MAGIC 0x52474d4fu               /* 'OMGR' */
#define GRN_VERSION 2
#define GRN_HEAD 64
#define GRN_GAIN_MAX 1024                   /* the tile's gain (+46); the level gains (+48) go up to 2047, checked on
                                             * the camera */
#define LOAD_DELAY_MS 300
#define LOAD_RETRY_MS 250

enum { LV_OFF, LV_LIST, LV_FLAT, LV_STEP, LV_FILM, LV_CHROMA, LV_N };
static const char *const lv_names[LV_N] = {"off", "list", "flat", "step", "film", "chroma"};
#define MASK_KEY 2                          /* the job key of the look's stills (Natural's) */
#define FILM_STEP 50                        /* the archive's bands: Y 0-50, 50-100, ..., their middles 25, 75, ... */
static const u16 film_w[5] = {481, 921, 1054, 962, 573};   /* x 1023: 0.47, 0.90, 1.03, 0.94, 0.56 (52 sec. 16) */

#ifdef OMK_DEBUG
#define BIS_STORE "GRAIN"
#define BIS_STORE_SIZE 128
#define BIS_KEY 1
#define BIS_LEVELS 8
#define BIS_JOBS_MAX 64
#define BIS_RETRY_MS 2000                   /* a failed save of the next level: tried again after this */
enum { RES_NONE, RES_OK, RES_RESET };

/* The bisection's state on the card (store GRAIN, key 1) */
typedef struct {
    u32 run;                                /* mask_run: another number starts the list again */
    u32 index;                              /* the level under test (an index of the list); the list's length - done */
    u32 done;                               /* our jobs ended at it */
    u32 saves;                              /* counts the forced saves: every one is a change the store writes */
    u32 armed;                              /* 1 - this run reached mode 1 with the level given to the driver */
    u32 result[BIS_LEVELS];                 /* RES_* << 16 | the jobs ended at the level */
} bisect_t;

static struct {
    omk_store *s;
    bisect_t b;
    u32 n, levels[BIS_LEVELS], jobs;
    u32 freed0, done0;                      /* the driver's count and b.done when the level began in this run */
    u32 retry_ms;
    int on, armed;
} bis;
#define BIS_ON (bis.on && bis.b.index < bis.n)   /* a level of the list is under test */
#else
#define BIS_ON 0
#endif

static struct {
    u32 level;                              /* `mask`: the level after the list (or without one) */
    u32 chroma, chroma_test;
    u32 level_test;                         /* [grain] level_test: the strength of a preset at Off, for tests (0 - none) */
    u32 route, ch, tag, given;              /* given to the driver */
    u32 curve, seq, warned;                 /* the curve published (LV_*, 0 none) and the tone it was built on */
    u16 w[256];
} mk;                                       /* mk_config fills the settings before the service starts */

typedef struct {
    char file[NAME];
    char name[24];
    u32 side, mean, crc, hcrc;
    u16 gain[OMK_GRAIN_LEVELS];             /* by strength; [0] Off = 0 */
} grn_t;

static grn_t *grn;
static u32 ngrn;
static omk_mod *gl_owner;                   /* the module that took the service in its module_init (grain_drop_module) */
static const char *grn_names[OMK_GRAIN_TILES];
static struct {
    int on;                                 /* the service is taken, the hooks are in */
    u8 *slot;
    u32 cap;                                /* the slot's size: the largest tile */
    s32 want, loaded, failed;               /* the tile chosen, the one in the slot (-1 none), one that did not load */
    u32 level, gain, large, look;
    u32 optics;                             /* the optics layer's step wanted on stills without grain (given to the driver) */
    u32 changed_ms, retry_ms;
    u32 entry;
    u32 unready;                            /* the slot is still marked ready with a tile no longer chosen (a fill was
                                               reading it when the choice changed): mark it at the next service */
} gl = {0, 0, 0, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static u32 rd16(const u8 *p)
{
    return p[0] | p[1] << 8;
}

static u32 rd32(const u8 *p)
{
    return rd16(p) | rd16(p + 2) << 16;
}

/* A container's header: 0, or why not. Fills g but the file name */
static const char *grn_header(const u8 *h, grn_t *g)
{
    static const u8 zero[4];
    u32 crc, side, i, k, gain;

    if (rd32(h) != GRN_MAGIC)
        return "not a grain container (OMGR)";
    if (rd16(h + 4) != GRN_VERSION || rd16(h + 6) != GRN_HEAD)
        return "not version 2";
    crc = crc32_update(0, h, 12);
    crc = crc32_update(crc, zero, 4);
    crc = crc32_update(crc, h + 16, GRN_HEAD - 16);
    if (crc != rd32(h + 12))
        return "header CRC mismatch";
    side = rd16(h + 40);
    if (side < 128 || side > 768 || side % 64 || DRV_GRAIN_TILE % side)
        return "tile side";
    if (rd32(h + 8) != GRN_HEAD + side * side)
        return "file size in the header";
    if (h[44] > DRV_GRAIN_TILE_MAX || h[45] || rd16(h + 54) || rd32(h + 60))
        return "largest value, flags or reserved fields";
    for (i = 0; i < 23 && h[16 + i]; i++)
        if (h[16 + i] < 32 || h[16 + i] > 126)
            return "name: not printable ASCII";
    if (!i || h[16 + 23] || h[16] == ' ' || h[16 + i - 1] == ' ')
        return "name: empty, too long or spaces at its ends";
    for (k = i; k < 24; k++)
        if (h[16 + k])
            return "name: not zero padded";
    gain = rd16(h + 46);
    if (!gain || gain > GRN_GAIN_MAX)
        return "tile gain";
    if (rd16(h + 48) || rd16(h + 50) || rd16(h + 52)) {     /* the gains of the strengths given */
        for (k = 0; k < 3; k++)
            if (!rd16(h + 48 + 2 * k) || rd16(h + 48 + 2 * k) > DRV_GRAIN_GAIN_MAX ||
                (k && rd16(h + 48 + 2 * k) < rd16(h + 46 + 2 * k)))
                return "strength gains: 1..2047, not falling";
        for (k = 0; k < 3; k++)
            g->gain[k + 1] = (u16)rd16(h + 48 + 2 * k);
    } else {                                /* the default: 0.25, 0.55 and 1 x the tile's gain, rounded */
        g->gain[1] = (u16)((gain * 25 + 50) / 100);
        g->gain[2] = (u16)((gain * 55 + 50) / 100);
        g->gain[3] = (u16)gain;
    }
    g->gain[0] = 0;
    for (k = 0; k < i; k++)
        g->name[k] = (char)h[16 + k];
    g->name[i] = 0;
    g->side = side;
    g->mean = (rd16(h + 42) + 128) >> 8;
    g->crc = rd32(h + 56);
    g->hcrc = crc;
    return 0;
}

static int grn_cmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (u8)*a - (u8)*b;
}

/* The containers' headers, in the order of their file names (upper case 8.3) */
static void grn_scan(void)
{
    static u8 entry[DRV_FIND_ENTRY];
    char path[OMK_PATH_MAX], n[NAME], file[NAME];
    u8 head[GRN_HEAD];
    grn_t t;
    const char *why;
    u32 k, j, seen = 0;
    void *fp;
    int h, rc;

    grn = mem_alloc(OMK_GRAIN_TILES * sizeof(grn_t));
    if (!grn || omk_path(path, TILE_DIR, "*.GRN"))
        return;
    mem_set(entry, 0, sizeof(entry));
    h = drv_findfirst(path, entry);
    if (h == -1 || !h)
        return;
    for (rc = 0; !rc && seen < 256; seen++) {
        const char *name = (const char *)entry + 24;
        u32 attr = entry[0] | entry[1] << 8;

        if (!(attr & 0x10) && name[0] != '.' && !omk_name83(n, name, 1)) {
            if (ngrn == OMK_GRAIN_TILES) {
                log_printf(OMK_LOG_WARN, "grain: more than %u containers, %s left out", OMK_GRAIN_TILES, n);
            } else {
                mem_copy(grn[ngrn].file, n, NAME);
                ngrn++;
            }
        }
        mem_set(entry, 0, sizeof(entry));
        rc = drv_findnext(h, entry);
    }
    drv_findclose(h);
    for (k = 1; k < ngrn; k++)              /* by name: a few dozen at most */
        for (j = k; j > 0 && grn_cmp(grn[j - 1].file, grn[j].file) > 0; j--) {
            mem_copy(&t, &grn[j], sizeof(t));
            mem_copy(&grn[j], &grn[j - 1], sizeof(t));
            mem_copy(&grn[j - 1], &t, sizeof(t));
        }
    for (k = 0, j = 0; k < ngrn; k++) {     /* the headers: a bad one is left out */
        mem_copy(file, grn[k].file, NAME);
        why = "cannot open";
        if (!omk_path(path, TILE_DIR, file) && (fp = drv_fopen(path, "r")) != 0) {
            why = drv_fread(head, 1, GRN_HEAD, fp) == GRN_HEAD ? grn_header(head, &t) : "shorter than its header";
            drv_fclose(fp);
        }
        if (why) {
            log_printf(OMK_LOG_WARN, "grain: %s: %s, left out", file, why);
            continue;
        }
        mem_copy(&grn[j], &t, sizeof(t));
        mem_copy(grn[j].file, file, NAME);
        grn_names[j] = grn[j].name;
        if (grn[j].side * grn[j].side > gl.cap)
            gl.cap = grn[j].side * grn[j].side;
        log_printf(OMK_LOG_INFO, "grain: tile %u: %s '%s', %u x %u, mean %u, gains %u %u %u", j + 1, file, grn[j].name,
                   grn[j].side, grn[j].side, grn[j].mean, grn[j].gain[1], grn[j].gain[2], grn[j].gain[3]);
        j++;
    }
    ngrn = j;
}

/* Tile k into the slot, checked (the header again, the pixels' CRC, nothing after them). 0, or -1 with the reason logged;
 * 1 - a power-off or a sleep began between two pieces (a tile is up to 576 KB, 4 KB a read - hundreds of milliseconds,
 * and the card-off gate waits for the core task one second at most): given up, to be loaded again later */
static int grn_load(u32 k, u32 *ms)
{
    char path[OMK_PATH_MAX];
    u8 head[GRN_HEAD];
    grn_t t;
    u32 n = grn[k].side * grn[k].side, got, i, j, crc = 0, max = 0, extra, t0 = omk_ms();
    const char *why = 0;
    void *fp;

    if (omk_path(path, TILE_DIR, grn[k].file) || !(fp = drv_fopen(path, "r"))) {
        log_printf(OMK_LOG_ERROR, "grain: %s: cannot open", grn[k].file);
        return -1;
    }
    if (drv_fread(head, 1, GRN_HEAD, fp) != GRN_HEAD || grn_header(head, &t) || t.hcrc != grn[k].hcrc)
        why = "the file changed since power-on";
    for (got = 0; !why && got < n; got += i) {
        if (!power_running()) {
            drv_fclose(fp);
            log_printf(OMK_LOG_DEBUG, "grain: %s: a power-off or a sleep began at %u of %u bytes, loaded again later",
                       grn[k].file, got, n);
            return 1;
        }
        i = n - got < CHUNK ? n - got : CHUNK;
        if (drv_fread(chunk, 1, i, fp) != i) {
            why = "short";
            break;
        }
        crc = crc32_update(crc, chunk, i);
        for (j = 0; j < i; j++)
            if (chunk[j] > max)
                max = chunk[j];
        mem_copy(gl.slot + got, chunk, i);
    }
    extra = why ? 0 : drv_fread(chunk, 1, 1, fp);
    drv_fclose(fp);
    if (!why && (extra || crc != grn[k].crc || max > DRV_GRAIN_TILE_MAX))
        why = extra ? "longer than its header says" : crc != grn[k].crc ? "pixels CRC mismatch" : "values above 63";
    *ms = omk_ms() - t0;
    if (why) {
        log_printf(OMK_LOG_ERROR, "grain: %s: %s, no grain with it", grn[k].file, why);
        return -1;
    }
    return 0;
}

/* The gain the look's stills get now, and the allocator's layout: to the driver when they change */
static void gl_publish(void)
{
    u32 gain = gl.want >= 0 && gl.level ? grn[gl.want].gain[gl.level] : 0;
    /* the optics layer on a still without grain: the mask route's list without the blend (its step is in that list) -
     * a tile is still needed (the list's texture is built, not blended) */
    u32 opt = !gain && gl.want >= 0 && optics_route(), large = gl.look && (gain || opt);

    if (gain != gl.gain) {
        gl.gain = gain;
        drv_grain_look_gain(gain);
    }
    if (opt != gl.optics) {
        gl.optics = opt;
        drv_grain_look_optics(opt);
        log_printf(OMK_LOG_INFO, "grain: %s", opt ? "the look's stills without grain take the mask route's list for the optics "
                   "layer (no grain blended)" : "no optics layer on the stills without grain");
    }
    if (large != gl.large) {
        gl.large = large;
        if (drv_alloc_large((int)large))
            log_printf(OMK_LOG_ERROR, "grain: allocator layout not changed");
        else
            log_printf(OMK_LOG_INFO, "grain: allocator %s (picture mode rebuilt at the next UI read)",
                       large ? "large layout (mode 1): the look's stills have grain" : "back to its own choice");
    }
}

/* ---- The tone mask: the level, its route and curve, the bisection over power-ons ---- */

static int lv_parse(const char *s, u32 n, u32 *lv)
{
    u32 k, i;

    for (k = 0; k < LV_N; k++) {
        for (i = 0; i < n && lv_names[k][i] && (s[i] | 0x20) == lv_names[k][i]; i++)
            ;
        if (i == n && !lv_names[k][i]) {
            *lv = k;
            return 0;
        }
    }
    return -1;
}

static u32 cfg_range(const char *key, int def, int lo, int hi)
{
    int v = cfg_int("grain", key, def);

    if (v < lo || v > hi) {
        log_printf(OMK_LOG_WARN, "grain: %s = %d out of %d..%d, %d taken", key, v, lo, hi, def);
        v = def;
    }
    return (u32)v;
}

/* [grain]: the working level, the chroma gains, the test list */
static void mk_config(void)
{
    const char *s = cfg_str("grain", "mask", "film");
    u32 lv, n;
#ifdef OMK_DEBUG
    const char *p, *q;
    char word[8];
#endif

    for (n = 0; s[n]; n++)
        ;
#ifdef OMK_DEBUG
    if (lv_parse(s, n, &lv) || lv == LV_CHROMA) {
        log_printf(OMK_LOG_WARN, "grain: mask = %s: not film, step, flat, list or off - film taken", s);
        lv = LV_FILM;
    }
#else
    if (lv_parse(s, n, &lv) || (lv != LV_FILM && lv != LV_FLAT && lv != LV_OFF)) {
        log_printf(OMK_LOG_WARN, "grain: mask = %s: not film, flat or off - film taken", s);
        lv = LV_FILM;
    }
#endif
    mk.level = lv;
    mk.chroma = cfg_range("chroma", 0, 0, DRV_GRAIN_CHROMA_MAX);
#ifdef OMK_DEBUG
    mk.chroma_test = cfg_range("chroma_test", 100, 0, DRV_GRAIN_CHROMA_MAX);
    mk.level_test = cfg_range("level_test", 0, 0, OMK_GRAIN_LEVELS - 1);
    if (mk.level_test)
        log_printf(OMK_LOG_WARN, "grain: level_test = %u: a preset with a tile and the strength Off gets strength %u on its "
                   "stills (a test key)", mk.level_test, mk.level_test);
    bis.jobs = cfg_range("mask_jobs", 4, 1, BIS_JOBS_MAX);
    for (p = cfg_str("grain", "mask_test", ""); *p;) {
        while (*p == ' ' || *p == ',' || *p == '\t')
            p++;
        for (q = p; *q && *q != ',' && *q != ' ' && *q != '\t'; q++)
            ;
        if (q == p)
            break;
        if (lv_parse(p, (u32)(q - p), &lv)) {
            for (n = 0; n < sizeof(word) - 1 && p + n < q; n++)
                word[n] = p[n];
            word[n] = 0;
            log_printf(OMK_LOG_WARN, "grain: mask_test: '%s' is not off, list, flat, step, film or chroma - left out", word);
        } else if (bis.n == BIS_LEVELS) {
            log_printf(OMK_LOG_WARN, "grain: mask_test: more than %u levels, the rest left out", BIS_LEVELS);
        } else {
            bis.levels[bis.n++] = lv;
        }
        p = q;
    }
#endif
}

static u32 mk_level(void)
{
#ifdef OMK_DEBUG
    if (BIS_ON)
        return bis.levels[bis.b.index];
#endif
    return mk.level;
}

static u32 mk_route(u32 lv)
{
    return lv == LV_OFF ? DRV_GRAIN_GF : lv == LV_LIST ? DRV_GRAIN_LIST : DRV_GRAIN_MASK;
}

/* The weight by the picture's Y after the tone, 0..1023 */
static void mk_curve(u32 lv, u16 *w)
{
    u32 y, k, x;
    s32 v;

    for (y = 0; y < 256; y++) {
        if (lv == LV_FLAT) {
            v = 1023;
        } else if (lv == LV_STEP) {
            v = y < 128 ? 0 : 1023;
        } else {                            /* film: constant outside the middles 25 and 225, linear between them */
            x = y < FILM_STEP / 2 ? 0 : y > FILM_STEP / 2 + 4 * FILM_STEP ? 4 * FILM_STEP : y - FILM_STEP / 2;
            k = x / FILM_STEP;
            v = k < 4 ? film_w[k] + ((s32)film_w[k + 1] - (s32)film_w[k]) * (s32)(x - k * FILM_STEP) / FILM_STEP :
                film_w[4];
        }
        w[y] = (u16)(v > 1023 ? 1023 : v);
    }
}

#ifdef OMK_DEBUG
/* The state to the card now: 0, or -1 (not written: the store keeps nothing newer than the card) */
static int bis_save(void)
{
    u32 keys, used, seq0, seq1;

    bis.b.saves++;                          /* a change every time: the store writes only changed data */
    if (store_set(bis.s, BIS_KEY, &bis.b, sizeof(bis.b), OMK_STORE_PERSIST))
        return -1;
    store_info(bis.s, &keys, &used, &seq0);
    store_save_now(bis.s);
    store_info(bis.s, &keys, &used, &seq1);
    return seq1 != seq0 ? 0 : -1;
}

static void bis_report(void)
{
    u32 i, r;

    for (i = 0; i < bis.n; i++) {
        r = bis.b.result[i];
        log_printf(r >> 16 == RES_RESET ? OMK_LOG_WARN : OMK_LOG_INFO, "grain: mask test step %u, %s: %s, %u jobs", i + 1,
                   lv_names[bis.levels[i]], r >> 16 == RES_OK ? "passed" : r >> 16 == RES_RESET ? "RESET the camera" :
                   i == bis.b.index ? "under test" : "not tested", i == bis.b.index ? bis.b.done : r & 0xffff);
    }
}

/* Boot, before the hooks: the level of this run on the card */
static void bis_start(u32 run)
{
    bisect_t b;

    if (!bis.n)
        return;
    bis.s = store_open(BIS_STORE, BIS_STORE_SIZE);
    if (!bis.s) {
        log_printf(OMK_LOG_ERROR, "grain: mask test: no store, the level is mask = %s", lv_names[mk.level]);
        return;
    }
    if (store_get(bis.s, BIS_KEY, &b, sizeof(b)) != (int)sizeof(b) || b.run != run || b.index > bis.n) {
        mem_set(&b, 0, sizeof(b));
        b.run = run;
        log_printf(OMK_LOG_INFO, "grain: mask test run %u starts", run);
    } else if (!core_prev_ended() && b.index < bis.n && b.armed) {
        b.result[b.index] = RES_RESET << 16 | b.done;
        log_printf(OMK_LOG_WARN, "grain: mask test: the run before did not end with a power-off at step %u (%s) after %u "
                   "jobs: marked as a reset", b.index + 1, lv_names[bis.levels[b.index]], b.done);
        b.index++;
        b.done = 0;
    } else if (!core_prev_ended() && b.index < bis.n) {
        log_printf(OMK_LOG_WARN, "grain: mask test: the run before did not end with a power-off before it could take a "
                   "still (boot interrupted): step %u (%s) is tested again", b.index + 1, lv_names[bis.levels[b.index]]);
    }
    b.armed = 0;
    mem_copy(&bis.b, &b, sizeof(b));
    if (bis_save()) {
        log_printf(OMK_LOG_ERROR, "grain: mask test: the state was not written to the card, the level is mask = %s",
                   lv_names[mk.level]);
        return;
    }
    bis.done0 = bis.b.done;
    bis.on = 1;
    bis_report();
    if (bis.b.index < bis.n)
        log_printf(OMK_LOG_INFO, "grain: mask test: this run tests step %u of %u (%s), %u of %u jobs done", bis.b.index + 1,
                   bis.n, lv_names[bis.levels[bis.b.index]], bis.b.done, bis.jobs);
    else
        log_printf(OMK_LOG_INFO, "grain: mask test run %u finished, the level is mask = %s", run, lv_names[mk.level]);
}
#endif

/* The level's route, chroma and curve to the drivers. A mask route is given only once its curve is in the ISP driver
 * (until then the grain film's list, untagged): a still never gets the look's tone as its mask */
static void mk_publish(void)
{
    u32 lv = mk_level(), route = mk_route(lv), ch, tag, seq = drv_isp_tone_seq(), curve;
    int rc;

    curve = route == DRV_GRAIN_MASK ? (lv == LV_CHROMA ? LV_FILM : lv) : 0;
    if (curve != mk.curve || (curve && seq != mk.seq)) {
        if (curve)
            mk_curve(curve, mk.w);
        rc = drv_isp_grain_mask(MASK_KEY, curve ? mk.w : 0);
        if (!rc) {
            if (curve != mk.curve)
                log_printf(OMK_LOG_INFO, "grain: mask curve %s (w at Y 0, 64, 128, 192, 255: %u %u %u %u %u)",
                           curve ? lv_names[curve] : "none", mk.w[0], mk.w[64], mk.w[128], mk.w[192], mk.w[255]);
            else
                log_printf(OMK_LOG_DEBUG, "grain: mask table built again for the look's tone");
            mk.curve = curve;
            mk.seq = seq;
        } else if (rc == -3 && !mk.warned) {
            log_printf(OMK_LOG_WARN, "grain: no ISP colour service (the look gave no tables): the mask route waits");
            mk.warned = 1;
        }
    }
    if (route == DRV_GRAIN_MASK && mk.curve != curve) {
        route = DRV_GRAIN_GF;               /* not yet */
        tag = 0;
    } else {
#ifdef OMK_DEBUG
        tag = BIS_ON ? bis.b.index + 1 : 0;
#else
        tag = 0;
#endif
    }
    ch = isp_mono() ? 0 : lv == LV_CHROMA ? mk.chroma_test : mk.chroma;
    if (!mk.given || route != mk.route || ch != mk.ch || tag != mk.tag) {
#ifdef OMK_DEBUG
        u32 freed, live;

        if (tag != mk.tag && !drv_grain_route_state(&freed, &live))
            bis.freed0 = freed;             /* the driver counts the releases of the tag in force */
#endif
        drv_grain_route(route, ch, tag);
        log_printf(OMK_LOG_INFO, "grain: the look's stills: route %s, level %s, chroma %u%s", route == DRV_GRAIN_GF ? "GF" :
                   route == DRV_GRAIN_LIST ? "list" : "mask", lv_names[lv], ch, tag ? " (mask test)" : "");
        mk.route = route;
        mk.ch = ch;
        mk.tag = tag;
        mk.given = 1;
    }
}

#ifdef OMK_DEBUG
/* Core loop: the level armed, its jobs counted; passed - the next one saved, then given (mk_publish) */
static void bis_service(u32 now)
{
    bisect_t prev;
    u32 freed, live, jobs;

    if (!bis.on || bis.b.index >= bis.n || mk.tag != bis.b.index + 1 || drv_grain_route_state(&freed, &live))
        return;
    if (!bis.armed) {                       /* a still at the level is possible from now on: a reset is the level's */
        if (drv_alloc_mode() != 1 || drv_card_busy() || (bis.retry_ms && now - bis.retry_ms < BIS_RETRY_MS))
            return;
        bis.b.armed = 1;
        if (bis_save()) {
            bis.b.armed = 0;
            store_set(bis.s, BIS_KEY, &bis.b, sizeof(bis.b), OMK_STORE_PERSIST);
            bis.retry_ms = now | 1;
            return;
        }
        bis.retry_ms = 0;
        bis.armed = 1;
        log_printf(OMK_LOG_INFO, "grain: mask test step %u (%s) armed", bis.b.index + 1, lv_names[mk_level()]);
    }
    jobs = bis.done0 + (freed - bis.freed0);
    if (jobs != bis.b.done) {
        bis.b.done = jobs;
        store_set(bis.s, BIS_KEY, &bis.b, sizeof(bis.b), OMK_STORE_PERSIST);   /* with the store's own delay */
    }
    if (jobs < bis.jobs || live || drv_card_busy() || (bis.retry_ms && now - bis.retry_ms < BIS_RETRY_MS))
        return;
    mem_copy(&prev, &bis.b, sizeof(prev));
    bis.b.result[bis.b.index] = RES_OK << 16 | jobs;
    bis.b.index++;
    bis.b.done = 0;
    if (bis_save()) {
        mem_copy(&bis.b, &prev, sizeof(prev));   /* the old level stays: nothing newer than the card is kept */
        store_set(bis.s, BIS_KEY, &bis.b, sizeof(bis.b), OMK_STORE_PERSIST);
        bis.retry_ms = now | 1;
        log_printf(OMK_LOG_WARN, "grain: mask test step %u passed, but the card did not take the state; tried again later",
                   prev.index + 1);
        return;
    }
    bis.retry_ms = 0;
    bis.done0 = 0;
    log_printf(OMK_LOG_INFO, "grain: mask test step %u (%s): passed, %u jobs; now %s", prev.index + 1,
               lv_names[bis.levels[prev.index]], jobs, bis.b.index < bis.n ? lv_names[bis.levels[bis.b.index]] :
               "the list is done");
    if (bis.b.index >= bis.n)
        bis_report();
    mk_publish();                           /* only now: a reset from here on is the new level's (armed stays saved) */
}
#endif

u32 grain_list(omk_mod *m, char (*files)[13], u32 max)
{
    drv_grain_cfg c;
    u32 i, total, used, largest;
    int rc;

    if (!m)
        return 0;
    if (!gl.on) {
        if (m->state != MOD_LOADING)
            return 0;
        grn_scan();
        if (!ngrn) {
            log_printf(OMK_LOG_INFO, "grain: no grain containers in OMKIT\\%s", TILE_DIR);
            return 0;
        }
        mem_arena(&total, &used, &largest);
        if (largest < gl.cap + ARENA_KEEP || !(gl.slot = mem_alloc(gl.cap))) {
            log_printf(OMK_LOG_ERROR, "grain: no room for the tile slot (%u bytes; arena largest free %u, %u kept)", gl.cap,
                       largest, ARENA_KEEP);
            ngrn = 0;
            return 0;
        }
        mk_config();
#ifdef OMK_DEBUG
        bis_start((u32)cfg_int("grain", "mask_run", 1));   /* on the card before the hooks can use a level */
#endif
        c.key = MASK_KEY;                   /* the look's stills: Natural's job key */
        c.type = 0x1000;                    /* stills (live view and video never) */
        log_checkpoint("grain: installing the look's grain hooks");
        rc = drv_grain_install(&c);
        if (rc) {
            log_printf(OMK_LOG_ERROR, "grain: the look's grain hooks not installed, step %d", rc);
            ngrn = 0;
            return 0;
        }
        rc = drv_look_grain_tiles(grn_names, ngrn);
        if (rc)
            log_printf(OMK_LOG_WARN, "grain: the grain item has no tiles, code %d", rc);
        gl.on = 1;
        log_printf(OMK_LOG_INFO, "grain: the look's grain by %s: %u tiles, slot %u bytes, allocator mode %u", m->name, ngrn,
                   gl.cap, drv_alloc_mode());
    }
    if (!gl_owner && m->state == MOD_LOADING)
        gl_owner = m;                       /* also after an owner that failed to start: the hooks and the slot stay */
    for (i = 0; files && i < ngrn && i < max; i++)
        mem_copy(files[i], grn[i].file, NAME);
    return ngrn;
}

/* The owner failed module_init: no grain on the stills from its choice (the gain 0, the allocator back to its own
 * layout, the slot's tile counted as gone), the grain item without tiles. The hooks and the slot stay for a module
 * loaded later */
void grain_drop_module(omk_mod *m)
{
    u32 gen;

    if (!gl.on || gl_owner != m)
        return;
    gl_owner = 0;
    gl.want = -1;
    gl.level = 0;
    gl.failed = -1;
    if (gl.loaded >= 0) {                   /* as in grain_look: not ready from now on */
        gl.unready = drv_grain_slot_begin(&gen) == -1;
        gl.loaded = -1;
    }
    gl_publish();
    drv_look_grain_tiles(0, 0);
    log_printf(OMK_LOG_WARN, "grain: %s failed to start, the look's grain is off", m->name);
}

/* The owner's choice for entry `entry` (look.c checks the owner) */
int grain_look(u32 entry, s32 tile, u32 level)
{
    u32 gen;
    int rc;

    if (!gl.on)
        return -2;
    if (tile < -1 || tile >= (s32)ngrn || level >= OMK_GRAIN_LEVELS)
        return -1;
    if (!level && tile >= 0)                /* [grain] level_test: grain stills for a test without the user's choice */
        level = mk.level_test;
    rc = drv_look_grain(entry, tile, level);
    if (rc)
        return rc;
    if (tile != gl.want) {
        gl.want = tile;
        gl.changed_ms = omk_ms();
        gl.failed = -1;
        if (gl.loaded >= 0 && tile != gl.loaded) {
            /* Not ready from now on: a still before the load has no grain (logged). The slot's tile counts as gone
             * even if the choice comes back to it before the load (A, B, A within the load delay): it is loaded
             * again, so the slot never stays marked not ready with the chosen tile "loaded". A fill reading the
             * slot now (-1): marked at the next service, before anything else */
            gl.unready = drv_grain_slot_begin(&gen) == -1;
            gl.loaded = -1;
        }
    }
    if (level != gl.level || entry != gl.entry)
        log_printf(OMK_LOG_DEBUG, "grain: entry %u: tile %d (%s), strength %u", entry + 1, tile + 1,
                   tile >= 0 ? grn[tile].name : "none", level);
    gl.level = level;
    gl.entry = entry;
    gl_publish();
    return 0;
}

static void gl_service(u32 now)
{
    u32 gen, ms;
    s32 k;
    int rc;

    if (!gl.on)
        return;
    gl.look = drv_look_active() != 0;
    gl_publish();
    if (gl.unready) {
        rc = drv_grain_slot_begin(&gen);
        if (rc == -1)
            return;                         /* still being read by a fill: the next service */
        gl.unready = 0;
    }
    k = gl.want;
    if (k < 0 || k == gl.loaded || k == gl.failed || now - gl.changed_ms < LOAD_DELAY_MS ||
        (gl.retry_ms && now - gl.retry_ms < LOAD_RETRY_MS))
        return;
    if (drv_card_busy()) {                  /* a picture is being written: the card after it */
        gl.retry_ms = now | 1;
        return;
    }
    rc = drv_grain_slot_begin(&gen);
    if (rc == -1) {                         /* a still's fill reads the slot now */
        gl.retry_ms = now | 1;
        return;
    }
    gl.retry_ms = 0;
    gl.loaded = -1;
    if (!rc)
        rc = grn_load((u32)k, &ms);
    if (rc > 0) {                           /* cut short by a power-off or a sleep: again when the camera runs (this
                                               function is not called before that), not a failed tile */
        gl.retry_ms = now | 1;
        return;
    }
    if (rc) {
        gl.failed = k;
        return;
    }
    drv_grain_slot_ready(gen, gl.slot, grn[k].side, grn[k].mean, grn[k].name);
    gl.loaded = k;
    log_printf(OMK_LOG_INFO, "grain: tile %s '%s' loaded in %u ms", grn[k].file, grn[k].name, ms);
}

void grain_service(u32 now)
{
    gl_service(now);
    if (!gl.on)
        return;
    mk_publish();
#ifdef OMK_DEBUG
    bis_service(now);
#endif
    drv_grain_service(now);
}

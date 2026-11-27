/* Host test of FILMLOOK's start (src/modules/filmlook/filmlook.c, compiled into this program): module_init and the first
 * ticks against a stand-in for core's API that records what the module asks for and in which order.
 *
 *   test_filmlook <work dir> <presets dir> <scenario>
 *
 * The work dir keeps the module's store between two runs of the program (a run is a power-on: the module's state is
 * static). Scenarios:
 *   first      no store: every preset read once (its cassette picture handed over from that one read), the CRCs are
 *              those of the bit-wise CRC-32, nothing given to the ISP service; then the user chooses the look and preset
 *              6 and moves the curve's shadows - the store is written;
 *   again      the look comes back: the stored preset's file is read first, the look is on and the tables (with the
 *              stored edit) are in force before any other preset is read; the first tick gives the same tables without
 *              reading the file again and publishes the entry, the editors and the "on" line;
 *   leftlook   as again, but the user left the look while the presets were read: off, the camera's tables back;
 *   notnatural as again, but core refuses to turn the look on (the stored picture mode changed): asked once, no tables;
 *   gone       as again, but the stored preset's file is not on the card: no early tables, the first preset instead;
 *   notlisted  as again, with `presets =` naming other files: no early tables, the list's first preset;
 *   format2    as again, the stored record made one of format 2 (FILMLOOK 1.5..1.12: no optics strengths, its last two
 *              bytes padding - set to garbage here): read with the optics rows at 0, the other edits kept;
 *   edits      (its own empty work dir) every preset chosen in turn and its curve edited: each block given to the ISP
 *              service is, byte for byte, the file's ISPB block with the edits applied (built here apart from the
 *              module's cached copies); an edit sooner than 41 ms after the last tables waits first and is built and
 *              given once; the editors' values without Color/Vivid differ from those with it in the colours only. */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filmlook.c"                       /* the module itself: its statics are checked below */

static int failed;

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);             \
            failed++;                                                           \
        }                                                                       \
    } while (0)

/* ---- what the module did, in order ---- */
enum { T_OPEN, T_LOOK_SET, T_OVERRIDE, T_STOCK, T_LIST, T_ART, T_EDITS, T_ENABLE, T_SCOPE, T_OPTICS };
static struct {
    int what;
    u32 arg;
    char name[OMK_NAME_MAX];
} trace[512];
static u32 ntrace;

static void note(int what, u32 arg, const char *name)
{
    if (ntrace == sizeof(trace) / sizeof(trace[0])) {
        failed++;
        return;
    }
    trace[ntrace].what = what;
    trace[ntrace].arg = arg;
    snprintf(trace[ntrace].name, sizeof(trace[ntrace].name), "%s", name ? name : "");
    ntrace++;
}

static u32 count_of(int what)
{
    u32 i, n = 0;

    for (i = 0; i < ntrace; i++)
        n += trace[i].what == what;
    return n;
}

static int first_of(int what)               /* its place in the trace, -1 none */
{
    u32 i;

    for (i = 0; i < ntrace; i++)
        if (trace[i].what == what)
            return (int)i;
    return -1;
}

static u32 opens_of(const char *name)
{
    u32 i, n = 0;

    for (i = 0; i < ntrace; i++)
        n += trace[i].what == T_OPEN && !strcmp(trace[i].name, name);
    return n;
}

/* ---- the stand-in for core ---- */
static const char *work_dir, *presets_dir, *hidden = "", *cfg_presets = "";
static char logtext[65536];
static int look_set_rc, look_active, look_leaves;   /* look_leaves: the user leaves the look before look_list */
static u8 blocks[4][ISPB_SIZE];             /* the blocks given to the ISP service, in order */
static u8 last_block[ISPB_SIZE];            /* the last one, whatever their number */
static u32 nblocks, now_ms = 1000;
static u32 pace, last_ok_ms, refused, slept_ms;   /* pace: the ISP service refuses tables sooner than 40 ms after the last */
static omk_look_edits last_edits;
static u8 last_optics[OMK_OPTICS_ROWS];     /* the last strengths given to core (look_optics) */
static u8 optics_of[OMK_LOOK_MAX][OMK_OPTICS_ROWS];    /* ... the last ones of each entry, and how often given */
static u32 optics_n[OMK_LOOK_MAX];
static void (*tick_fn)(void *);

static void f_log(omk_mod *m, int level, const char *fmt, ...)
{
    size_t n = strlen(logtext);
    va_list ap;

    (void)m;
    (void)level;
    va_start(ap, fmt);
    vsnprintf(logtext + n, sizeof(logtext) - n - 1, fmt, ap);
    va_end(ap);
    strcat(logtext, "\n");
}

static const char *f_cfg_str(omk_mod *m, const char *key, const char *def)
{
    (void)m;
    return !strcmp(key, "presets") ? cfg_presets : def;
}

static int f_cfg_bool(omk_mod *m, const char *key, int def)
{
    (void)m;
    (void)key;
    return def;
}

static void *f_alloc(omk_mod *m, u32 size)
{
    (void)m;
    return malloc(size);
}

static void f_free(omk_mod *m, void *p)
{
    (void)m;
    free(p);
}

/* The store: records {key, length, bytes} in memory, the whole of it in <work dir>/store.bin */
static struct {
    u16 key;
    u16 len;
    u8 data[256];
} recs[80];
static u32 nrecs;

static void store_path(char *p, size_t n)
{
    snprintf(p, n, "%s/store.bin", work_dir);
}

static omk_store *f_store_open(omk_mod *m, u32 size)
{
    char p[600];
    FILE *f;

    (void)m;
    (void)size;
    store_path(p, sizeof(p));
    f = fopen(p, "rb");
    if (f) {
        if (fread(&nrecs, sizeof(nrecs), 1, f) != 1 || fread(recs, sizeof(recs[0]), nrecs, f) != nrecs)
            nrecs = 0;
        fclose(f);
    }
    return (omk_store *)recs;
}

static void store_write(void)
{
    char p[600];
    FILE *f;

    store_path(p, sizeof(p));
    f = fopen(p, "wb");
    fwrite(&nrecs, sizeof(nrecs), 1, f);
    fwrite(recs, sizeof(recs[0]), nrecs, f);
    fclose(f);
}

static int f_store_get(omk_store *s, u16 key, void *out, u32 max)
{
    u32 i;

    (void)s;
    for (i = 0; i < nrecs; i++)
        if (recs[i].key == key) {
            memcpy(out, recs[i].data, recs[i].len < max ? recs[i].len : max);
            return recs[i].len;
        }
    return -1;
}

static int f_store_set(omk_store *s, u16 key, const void *data, u32 len, u32 flags)
{
    u32 i;

    (void)s;
    (void)flags;
    for (i = 0; i < nrecs && recs[i].key != key; i++)
        ;
    if (i == sizeof(recs) / sizeof(recs[0]) || len > sizeof(recs[0].data))
        return -1;
    recs[i].key = key;
    recs[i].len = (u16)len;
    memcpy(recs[i].data, data, len);
    if (i == nrecs)
        nrecs++;
    return 0;
}

static int f_store_del(omk_store *s, u16 key)
{
    u32 i;

    (void)s;
    for (i = 0; i < nrecs; i++)
        if (recs[i].key == key) {
            recs[i] = recs[--nrecs];
            return 0;
        }
    return -1;
}

static omk_file *f_file_open(omk_mod *m, const char *name, const char *mode)
{
    char p[600];

    (void)m;
    (void)mode;
    note(T_OPEN, 0, name);
    if (!strcmp(name, hidden))
        return 0;
    snprintf(p, sizeof(p), "%s/%s", presets_dir, name);
    return (omk_file *)fopen(p, "rb");
}

static u32 f_file_read(omk_file *f, void *out, u32 len)
{
    return (u32)fread(out, 1, len, (FILE *)f);
}

static void f_file_close(omk_file *f)
{
    fclose((FILE *)f);
}

static u32 f_file_list_long(omk_mod *m, const char *pattern, char (*names)[OMK_NAME_MAX], u32 max)
{
    DIR *d = opendir(presets_dir);
    struct dirent *e;
    u32 n = 0;
    size_t l;

    (void)m;
    (void)pattern;
    while (d && (e = readdir(d)) != 0) {
        l = strlen(e->d_name);
        if (l > 4 && l < OMK_NAME_MAX && !strcmp(e->d_name + l - 4, ".ISP") && strcmp(e->d_name, hidden) && n < max)
            strcpy(names[n++], e->d_name);
    }
    if (d)
        closedir(d);
    return n;
}

static const void *f_res(omk_mod *m, u32 type, u32 id, u32 *size)
{
    (void)m;
    (void)type;
    (void)id;
    *size = 0;                              /* no pictures in this "module file" */
    return 0;
}

static void f_sleep(u32 ms)
{
    now_ms += ms;
    slept_ms += ms;
}

static u32 f_ms(void)
{
    return now_ms;
}

static int f_isp_override(omk_mod *m, const void *block, u32 size)
{
    (void)m;
    if (!block) {
        note(T_STOCK, 0, 0);
        return 0;
    }
    if (pace && last_ok_ms && now_ms - last_ok_ms < 40) {
        refused++;
        return -2;                          /* too soon, as core's fast class */
    }
    last_ok_ms = now_ms;
    note(T_OVERRIDE, size, 0);
    if (nblocks < 4 && size == ISPB_SIZE)
        memcpy(blocks[nblocks], block, size);
    if (size == ISPB_SIZE)
        memcpy(last_block, block, size);
    nblocks++;
    return 0;
}

static int f_isp_scope(omk_mod *m, u32 scope)
{
    (void)m;
    note(T_SCOPE, scope, 0);
    return 0;
}

static int f_look_enable(omk_mod *m, const char *slot, omk_look_fn fn, void *ctx)
{
    (void)m;
    (void)fn;
    (void)ctx;
    note(T_ENABLE, 0, slot);
    return 0;
}

static int f_look_list(omk_mod *m, const char *const *n, u32 cnt, u32 cur)
{
    (void)m;
    (void)n;
    note(T_LIST, cnt << 8 | cur, 0);
    return 0;
}

static int f_look_set(omk_mod *m, int on)
{
    (void)m;
    note(T_LOOK_SET, (u32)on, 0);
    if (on && look_set_rc)
        return look_set_rc;
    look_active = on;
    return 0;
}

static int f_look_get(void)
{
    return look_active && !look_leaves;
}

static int f_look_picture_size(u32 pic, u32 *w, u32 *h)
{
    (void)pic;
    *w = 66;
    *h = 50;
    return 0;
}

static int f_every(omk_mod *m, u32 ms, void (*fn)(void *ctx), void *ctx)
{
    (void)m;
    (void)ms;
    (void)ctx;
    tick_fn = fn;
    return 0;
}

static int f_exif(omk_mod *m, const char *text)
{
    (void)m;
    (void)text;
    return 0;
}

static int f_look_name(omk_mod *m, const char *text)
{
    (void)m;
    (void)text;
    return 0;
}

static int f_look_art(omk_mod *m, u32 index, const void *data, u32 size)
{
    (void)m;
    (void)data;
    note(T_ART, index, 0);
    return size ? 0 : -1;
}

static int f_look_edits(omk_mod *m, const omk_look_edits *e)
{
    (void)m;
    note(T_EDITS, e->entry, 0);
    last_edits = *e;
    return 0;
}

/* The optics strengths at the module's start (FILMLOOK 1.13): every preset's given once - the rows show a preset's at
 * once when a dial chooses it, before its apply - and the chosen one's last (core keeps the last as the chosen's) */
static int optics_all_given(u32 chosen)
{
    int i, last = -1;
    u32 k;

    for (i = 0; i < (int)ntrace; i++)
        if (trace[i].what == T_OPTICS)
            last = i;
    for (k = 0; k < count; k++)
        if (optics_n[k] != 1)
            return 0;
    return count_of(T_OPTICS) == count && last >= 0 && trace[last].arg == chosen && first_of(T_OPTICS) > first_of(T_LIST);
}

static int f_look_optics(omk_mod *m, u32 entry, const u8 *strength, u32 n)
{
    (void)m;
    if (n != OMK_OPTICS_ROWS)
        return -1;
    note(T_OPTICS, entry, 0);
    memcpy(last_optics, strength, OMK_OPTICS_ROWS);
    if (entry < OMK_LOOK_MAX) {
        memcpy(optics_of[entry], strength, OMK_OPTICS_ROWS);
        optics_n[entry]++;
    }
    return 0;
}

static u32 f_grain_list(omk_mod *m, char (*files)[13], u32 max)
{
    (void)m;
    (void)files;
    (void)max;
    return 0;
}

static omk_api fake;

static void fake_init(void)
{
    memset(&fake, 0, sizeof(fake));         /* anything FILMLOOK should not call stays 0: a crash is a finding */
    fake.version = OMK_API_VERSION;
    fake.size = sizeof(fake);
    fake.log = f_log;
    fake.cfg_str = f_cfg_str;
    fake.cfg_bool = f_cfg_bool;
    fake.alloc = f_alloc;
    fake.free = f_free;
    fake.store_open = f_store_open;
    fake.store_get = f_store_get;
    fake.store_set = f_store_set;
    fake.store_del = f_store_del;
    fake.file_open = f_file_open;
    fake.file_read = f_file_read;
    fake.file_close = f_file_close;
    fake.file_list_long = f_file_list_long;
    fake.res = f_res;
    fake.sleep = f_sleep;
    fake.ms = f_ms;
    fake.isp_override = f_isp_override;
    fake.isp_scope = f_isp_scope;
    fake.look_enable = f_look_enable;
    fake.look_list = f_look_list;
    fake.look_set = f_look_set;
    fake.look_get = f_look_get;
    fake.look_picture_size = f_look_picture_size;
    fake.every = f_every;
    fake.exif_comment = f_exif;
    fake.look_name = f_look_name;
    fake.look_art = f_look_art;
    fake.look_edits = f_look_edits;
    fake.grain_list = f_grain_list;
    fake.look_optics = f_look_optics;
}

/* ---- the reference: the bit-wise CRC-32 FILMLOOK had ---- */
static u32 crc_bitwise(const u8 *p, u32 n)
{
    u32 crc = ~0u, k;

    while (n--) {
        crc ^= *p++;
        for (k = 0; k < 8; k++)
            crc = crc & 1 ? crc >> 1 ^ 0xedb88320u : crc >> 1;
    }
    return ~crc;
}

static u32 file_crc(const char *name)
{
    static u8 b[MAX_FILE];
    char p[600];
    FILE *f;
    u32 n;

    snprintf(p, sizeof(p), "%s/%s", presets_dir, name);
    f = fopen(p, "rb");
    if (!f)
        return 0;
    n = (u32)fread(b, 1, sizeof(b), f);
    fclose(f);
    return crc_bitwise(b, n);
}

#define CHOSEN 5                            /* the preset the first run leaves chosen (the sixth by name) */

static void run_first(void)
{
    u32 i, k;

    CHECK(module_init(&fake, (omk_mod *)&fake) == 0);
    CHECK(count >= 8 && count == count_of(T_OPEN));     /* every file read once, and no other read */
    CHECK(arts == count && count_of(T_ART) == count);   /* the bundled presets all have a cassette picture */
    for (i = 0; i < count; i++) {
        CHECK(opens_of(presets[i].file) == 1);
        CHECK(presets[i].crc == file_crc(presets[i].file));
        CHECK(i == 0 || cmp(presets[i - 1].file, presets[i].file) < 0);
    }
    for (i = 0, k = 0; i < ntrace; i++)     /* the pictures in the order of the list: entry 0, 1, ... */
        if (trace[i].what == T_ART)
            CHECK(trace[i].arg == k++);
    CHECK(first_of(T_ENABLE) >= 0 && first_of(T_ENABLE) < first_of(T_OPEN));   /* the services before the files */
    CHECK(!count_of(T_OVERRIDE) && !count_of(T_LOOK_SET) && !count_of(T_STOCK));
    CHECK(count_of(T_LIST) == 1 && trace[first_of(T_LIST)].arg == (count << 8 | 0));
    CHECK(pending == NOTHING && tick_fn == tick);
    CHECK(optics_all_given(0));             /* no store: the first preset chosen */
    CHECK(strstr(logtext, " presets in OMKIT\\FILMLOOK\\") && strstr(logtext, " cassette pictures, 0 presets without one"));

    /* the user chooses the look with preset CHOSEN + 1 and moves the shadows of its curve */
    look_active = 1;
    on_look(OMK_LOOK_ON, CHOSEN, 0);
    CHECK(count_of(T_OVERRIDE) == 1 && opens_of(presets[CHOSEN].file) == 2 && base_of == CHOSEN);
    now_ms += 100;
    on_look(OMK_LOOK_EDIT, OMK_EDIT_EVENT(OMK_EDIT_TONE, 0, 3), 0);
    tick(0);
    CHECK(count_of(T_OVERRIDE) == 2 && presets[CHOSEN].e.tone[0] == 3 && presets[CHOSEN].key);
    CHECK(memcmp(blocks[0], blocks[1], ISPB_SIZE) != 0);       /* the edit changes the tables */
    /* the optics rows: shown (their group with the editors), the chosen preset's strengths given to core at its apply;
     * Bloom 3 - kept with the preset's edits and given to core at once, the tables not touched; a strength past the top
     * or a row past the last is not taken */
    CHECK(optics_on && last_edits.groups >> OMK_EDIT_OPTICS & 1 && count_of(T_OPTICS) == count + 2);   /* start, on, edit */
    CHECK(!last_optics[0] && !last_optics[1]);
    on_look(OMK_LOOK_EDIT, OMK_EDIT_EVENT(OMK_EDIT_OPTICS, OMK_OPTICS_BLOOM, 3), 0);
    on_look(OMK_LOOK_EDIT, OMK_EDIT_EVENT(OMK_EDIT_OPTICS, OMK_OPTICS_HALATION, 6), 0);
    on_look(OMK_LOOK_EDIT, OMK_EDIT_EVENT(OMK_EDIT_OPTICS, 2, 1), 0);
    tick(0);
    CHECK(presets[CHOSEN].e.optics[OMK_OPTICS_BLOOM] == 3 && !presets[CHOSEN].e.optics[OMK_OPTICS_HALATION]);
    CHECK(count_of(T_OPTICS) == count + 3 && last_optics[OMK_OPTICS_BLOOM] == 3 && !last_optics[OMK_OPTICS_HALATION]);
    CHECK(count_of(T_OVERRIDE) == 2 && pending == NOTHING);
    CHECK(strstr(logtext, "preset 6: bloom 3") && !strstr(logtext, "halation 6"));
    store_write();
}

/* The block the ISP service must have got for preset i: its file's ISPB section with the preset's edits, built here
 * from the file - not from the module's base and work copies */
static void check_block(u32 i)
{
    static u8 file[MAX_FILE], ref[ISPB_SIZE];
    const u8 *ispb, *cani;
    char p[600];
    FILE *f;
    u32 n, ispb_n = 0, cani_n;

    snprintf(p, sizeof(p), "%s/%s", presets_dir, presets[i].file);
    f = fopen(p, "rb");
    CHECK(f != 0);
    if (!f)
        return;
    n = (u32)fread(file, 1, sizeof(file), f);
    fclose(f);
    CHECK(!isp_check(file, n, &ispb, &ispb_n, &cani, &cani_n) && ispb_n == ISPB_SIZE);
    if (ispb_n != ISPB_SIZE)
        return;
    memcpy(ref, ispb, ISPB_SIZE);
    edits_apply(ref, &presets[i].e);
    if (memcmp(ref, last_block, ISPB_SIZE)) {
        fprintf(stderr, "preset %u (%s): the block given differs from the file's with the edits\n", i + 1, presets[i].file);
        failed++;
    }
}

static void run_edits(void)
{
    static const signed char steps[][3] = {{3, 0, 0}, {3, -2, 0}, {3, -2, 7}, {-7, 5, -1}, {0, 0, 0}};
    omk_look_edits with, without;
    u32 i, k, j, n, t;

    CHECK(module_init(&fake, (omk_mod *)&fake) == 0);
    CHECK(count >= 8 && edits_on);
    CHECK(!((uptr)base & 3) && !((uptr)work & 3) && !(ISPB_SIZE & 3));   /* the blocks are copied by words */
    look_active = 1;
    on_look(OMK_LOOK_ON, 0, 0);
    CHECK(count_of(T_OVERRIDE) == 1);
    check_block(0);
    for (i = 0; i < count; i++) {
        ntrace = 0;                         /* the trace holds 512 notes: counted from here */
        now_ms += 2000;
        on_look(OMK_LOOK_ENTRY, i, 0);      /* the dial: the tables at once */
        CHECK(count_of(T_OVERRIDE) == 1 && base_of == i && pending == NOTHING);
        check_block(i);
        for (k = 0; k < sizeof(steps) / sizeof(steps[0]); k++) {
            for (j = 0; j < 3; j++) {       /* a click of each point, then one tick: applied once */
                now_ms += 100;
                on_look(OMK_LOOK_EDIT, OMK_EDIT_EVENT(OMK_EDIT_TONE, j, steps[k][j]), 0);
            }
            n = count_of(T_OVERRIDE);
            tick_fn(0);
            CHECK(presets[i].e.tone[0] == steps[k][0] && presets[i].e.tone[1] == steps[k][1] && presets[i].e.tone[2] == steps[k][2]);
            CHECK(count_of(T_OVERRIDE) == n + 1 && pending == NOTHING);
            check_block(i);
            CHECK(last_edits.entry == i && last_edits.tone[0] == steps[k][0]);
        }
    }
    /* The pace: the service takes tables 40 ms apart. An edit 10 ms after the last tables: the module waits out the
     * rest of its 41 ms first, then builds and gives the block once - never refused, never built twice */
    ntrace = 0;
    pace = 1;
    last_ok_ms = now_ms;
    applied_ms = now_ms;
    for (k = 0; k < 5; k++) {
        now_ms += 10;
        t = now_ms;
        n = count_of(T_OVERRIDE);
        slept_ms = 0;
        on_look(OMK_LOOK_EDIT, OMK_EDIT_EVENT(OMK_EDIT_TONE, 1, (int)k - 2), 0);
        CHECK(pending == APPLY && edit_only);
        tick_fn(0);
        CHECK(pending == NOTHING && !refused && count_of(T_OVERRIDE) == n + 1);
        CHECK(slept_ms == 31 && now_ms == t + 31);
        check_block(count - 1);
    }
    now_ms += 500;                          /* a late edit: no wait */
    slept_ms = 0;
    on_look(OMK_LOOK_EDIT, OMK_EDIT_EVENT(OMK_EDIT_TONE, 2, 4), 0);
    tick_fn(0);
    CHECK(!slept_ms && !refused && pending == NOTHING);
    check_block(count - 1);

    /* The editors' values: without Color/Vivid among the groups only the vertices' colours are left out */
    edits_show(base, work, &presets[count - 1].e, 1u << OMK_EDIT_TONE | 1u << OMK_EDIT_VIVID, &with);
    edits_show(base, work, &presets[count - 1].e, 1u << OMK_EDIT_TONE, &without);
    CHECK(!memcmp(with.curve, without.curve, sizeof(with.curve)) && !memcmp(with.tone, without.tone, sizeof(with.tone)));
    CHECK(with.vivid_mask == without.vivid_mask && !memcmp(with.vivid_hue, without.vivid_hue, sizeof(with.vivid_hue)));
    CHECK(!memcmp(with.vivid_base, without.vivid_base, sizeof(with.vivid_base)));
    CHECK(!memcmp(with.vivid, without.vivid, sizeof(with.vivid)) && with.filter == without.filter && with.level == without.level);
    for (i = 0, n = 0, t = 0; i < 12; i++)
        for (j = 0; j < 3; j++) {
            n += with.vivid_rgb[i][j] != 0;
            t += without.vivid_rgb[i][j] != 0;
        }
    CHECK(n > 0 && t == 0);
}

/* A later power-on with the look on in the store */
static void run_again(const char *what)
{
    int notnatural = !strcmp(what, "notnatural"), gone = !strcmp(what, "gone");
    int notlisted = !strcmp(what, "notlisted"), format2 = !strcmp(what, "format2");
    int early_want = !notnatural && !gone && !notlisted;
    char saved[OMK_NAME_MAX] = "";
    u32 i, opens;
    int at;

    f_store_open(0, 0);
    CHECK(f_store_get(0, KEY_PRESET, saved, sizeof(saved)) > 0);
    if (notnatural)
        look_set_rc = -4;
    if (gone)
        hidden = saved;
    if (notlisted)
        cfg_presets = "TRIX, GOLD";
    if (format2) {                          /* the record as FILMLOOK 1.5..1.12 wrote it, its padding not zero */
        for (i = 0; i < nrecs && recs[i].key < KEY_EDIT; i++)
            ;
        CHECK(i < nrecs && recs[i].len == sizeof(edit_rec));
        if (i < nrecs) {
            edit_rec *r = (edit_rec *)(void *)recs[i].data;

            CHECK(r->e.format == EDITS_FORMAT && r->e.optics[OMK_OPTICS_BLOOM] == 3);
            r->e.format = 2;
            r->e.optics[0] = 7;
            r->e.optics[1] = 9;
        }
        store_write();
    }
    CHECK(module_init(&fake, (omk_mod *)&fake) == 0);
    CHECK(count_of(T_LIST) == 1);
    if (early_want) {
        /* the stored preset's file is the first one opened; the look and its tables come before any other file */
        CHECK(trace[first_of(T_OPEN)].what == T_OPEN && !strcmp(trace[first_of(T_OPEN)].name, saved));
        at = first_of(T_OVERRIDE);
        CHECK(at > 0 && first_of(T_LOOK_SET) < at && first_of(T_LOOK_SET) > first_of(T_OPEN));
        for (i = 0, opens = 0; (int)i < at; i++)
            opens += trace[i].what == T_OPEN;
        CHECK(opens == 1);
        CHECK(count_of(T_LOOK_SET) == 1 && count_of(T_OVERRIDE) == 1);
        CHECK(count == count_of(T_OPEN) - 1 && opens_of(saved) == 2);
        CHECK(current == CHOSEN && !strcmp(presets[current].file, saved) && base_of == current);
        CHECK(presets[current].e.tone[0] == 3);            /* the stored edit is back, by the file's name and CRC */
        CHECK(presets[current].e.optics[OMK_OPTICS_BLOOM] == (format2 ? 0 : 3) && !presets[current].e.optics[1]);
        CHECK(trace[first_of(T_LIST)].arg == (count << 8 | CHOSEN));
        CHECK(optics_all_given(CHOSEN));    /* the stored strengths: the chosen preset's, the others 0 */
        for (i = 0; i < count; i++)
            CHECK(optics_of[i][OMK_OPTICS_BLOOM] == (i == CHOSEN && !format2 ? 3 : 0) && !optics_of[i][OMK_OPTICS_HALATION]);
        CHECK(active == 1 && pending == APPLY);
        CHECK(strstr(logtext, "its tables are on, before the presets are read"));
        CHECK(strstr(logtext, "edits of 1 presets restored") && strstr(logtext, "look restored, preset 6 '"));
        CHECK(!strstr(logtext, ") on, "));                 /* the preset's "on" line comes with the first tick */
        opens = count_of(T_OPEN);
        tick_fn(0);
        CHECK(pending == NOTHING && count_of(T_OPEN) == opens);    /* no file read for the first apply */
        CHECK(count_of(T_OVERRIDE) == 2 && !memcmp(blocks[0], blocks[1], ISPB_SIZE));   /* the same tables, edited */
        CHECK(count_of(T_EDITS) == 1 && last_edits.entry == CHOSEN && last_edits.tone[0] == 3);
        CHECK(count_of(T_OPTICS) == count + 1 && optics_n[CHOSEN] == 2 &&   /* at the start and the first tick */
              last_optics[OMK_OPTICS_BLOOM] == (format2 ? 0 : 3) && !last_optics[OMK_OPTICS_HALATION]);
        CHECK(strstr(logtext, "preset 6 '") && strstr(logtext, ") on, edited"));
        return;
    }
    CHECK(!strstr(logtext, "its tables are on, before the presets are read"));
    if (notnatural) {                       /* the file was read, core said no: asked once, nothing given */
        CHECK(count_of(T_LOOK_SET) == 1 && !count_of(T_OVERRIDE) && !count_of(T_STOCK));
        CHECK(active == 0 && pending == NOTHING && strstr(logtext, "look not restored, code -4"));
        CHECK(f_store_get(0, KEY_ACTIVE, &i, 4) == 4 && i == 0);
        return;
    }
    /* gone, notlisted: nothing before the list; the look comes back with the first preset of the list */
    CHECK(first_of(T_OVERRIDE) < 0 && count_of(T_LOOK_SET) == 1 && first_of(T_LOOK_SET) > first_of(T_LIST));
    CHECK(current == 0 && active == 1 && pending == APPLY && base_of != current);
    CHECK(notlisted ? count == 2 && !opens_of(saved) : opens_of(saved) == 1);
    tick_fn(0);
    CHECK(count_of(T_OVERRIDE) == 1 && base_of == 0 && opens_of(presets[0].file) == 2);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: test_filmlook <work dir> <presets dir> <scenario>\n");
        return 2;
    }
    work_dir = argv[1];
    presets_dir = argv[2];
    fake_init();
    if (!strcmp(argv[3], "first")) {
        run_first();
    } else if (!strcmp(argv[3], "edits")) {
        run_edits();
    } else if (!strcmp(argv[3], "leftlook")) {
        /* the user leaves the look while the presets are read: the stand-in's look_get says so from the start (the
         * module asks only at the end of module_init) */
        look_leaves = 1;
        CHECK(module_init(&fake, (omk_mod *)&fake) == 0);
        CHECK(count_of(T_LOOK_SET) == 1 && count_of(T_OVERRIDE) == 1);     /* the early restore happened ... */
        CHECK(active == 0 && pending == STOCK);                            /* ... and is taken back */
        CHECK(strstr(logtext, "look turned off while the presets were read") && !strstr(logtext, "look restored"));
        tick_fn(0);
        CHECK(count_of(T_STOCK) == 1 && count_of(T_OVERRIDE) == 1 && pending == NOTHING);
    } else {
        run_again(argv[3]);
    }
    if (failed)
        fprintf(stderr, "---- the module's log ----\n%s", logtext);
    else
        printf("ok\n");
    return failed != 0;
}

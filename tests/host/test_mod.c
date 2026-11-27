/* Module loader on the host: a real HELLO.OMM (built by GCC for MN10300); module code is not executed:
 * module_call is replaced with a C function that checks the address relocation and uses the API like a module.
 * Run: test_mod <empty directory> <HELLO.OMM>; tests/host/run.sh runs it (make test) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "omk_core.h"
#include "omk_log.h"
#include "omk_mem.h"
#include "omk_mod.h"
#include "omk_power.h"
#include "omk_prof.h"
#include "omk_store.h"
#include "omk_ev.h"
#include "omk_isp.h"
#include "host_drv.h"

static int failed;

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);             \
            failed++;                                                           \
        }                                                                       \
    } while (0)

/* A check of something only a debug build has (a line of the debug level, the rescue, the clock correction): the programs
 * are built and run for both kinds (OMKit/Makefile, BUILD) */
#ifdef OMK_DEBUG
#define CHECK_DEBUG(x) CHECK(x)
#else
#define CHECK_DEBUG(x) ((void)0)
#endif

static char trace[64];
static u32 ntrace, orig_calls, deferred, after_allowed;
static const omk_api *the_api;

static void orig_pm(void *data, void *ctx);

static void mark(char c)
{
    if (ntrace < sizeof(trace) - 1)
        trace[ntrace++] = c;
    trace[ntrace] = 0;
}

static void h_notify_before(omk_ev *ev, void *data, void *ctx)
{
    omk_ev_pictmode_check *d = data;

    (void)ev;
    (void)ctx;
    mark('n');
    d->mode = 1234;                         /* a copy: the original data does not change */
}

static void h_replace_hi(omk_ev *ev, void *data, void *ctx)
{
    omk_ev_pictmode_check *d = data;

    (void)ctx;
    mark('R');
    if (d->mode == 5) {                     /* decided on its own: the original is not called */
        d->allowed = 0;
        return;
    }
    if (d->mode == 9)                       /* re-entry into the same event: the original only */
        ev_dispatch(OMK_EV_PICTMODE_CHECK, d, orig_pm, 0);
    if (d->mode == 11) {                    /* another task gets to the hook while this one is in its handlers */
        host_task++;
        CHECK(ev_dispatch(OMK_EV_PICTMODE_CHECK, d, orig_pm, 0) == 1);
        host_task--;
    }
    the_api->next(ev);
}

static void h_replace_lo(omk_ev *ev, void *data, void *ctx)
{
    (void)data;
    (void)ctx;
    mark('r');
    the_api->next(ev);
}

static void h_filter_after(omk_ev *ev, void *data, void *ctx)
{
    omk_ev_pictmode_check *d = data;

    (void)ev;
    (void)ctx;
    mark('f');
    if (d->mode == 7)
        d->allowed = 0;
}

static void job(void *arg)
{
    deferred += (u32)(uptr)arg;
}

static u32 again_runs, again_stop;

/* A deferred call that defers itself, as a module polling for something would */
static void again(void *arg)
{
    again_runs++;
    if (!again_stop)
        CHECK(the_api->defer((omk_mod *)arg, again, arg) == 0);
}

static void h_notify_after(omk_ev *ev, void *data, void *ctx)
{
    omk_ev_pictmode_check *d = data;

    (void)ev;
    mark('a');
    after_allowed = d->allowed;
    the_api->defer((omk_mod *)ctx, job, (void *)3);
}

static void h_s1(omk_ev *ev, void *data, void *ctx)
{
    (void)ev;
    (void)data;
    (void)ctx;
    mark('s');
}

static void orig_pm(void *data, void *ctx)
{
    omk_ev_pictmode_check *d = data;

    (void)ctx;
    mark('O');
    orig_calls++;
    if (d)
        d->allowed = 1;
}

static unsigned char *orig;
static long orig_len;
static int calls, fail_rc;
static int off_in_init;                 /* fake_init: a power-off begins while the module starts */
static u32 power_seen[8], power_ctx_ok;
static u32 prof_seen, prof_calls;

static void h_prof(u32 prof, void *ctx)
{
    prof_calls++;
    prof_seen = prof;
    CHECK(ctx == &prof_seen);
}

static void h_power(u32 event, void *ctx)
{
    if (event < 8)
        power_seen[event]++;
    power_ctx_ok += ctx == &power_seen;
}

static void write_bin(const char *rel, const void *data, long n)
{
    char p[512];
    FILE *f;

    snprintf(p, sizeof(p), "%s/%s", host_dir, rel);
    f = fopen(p, "wb");
    fwrite(data, 1, n, f);
    fclose(f);
}

static char *read_text(const char *rel)
{
    char p[512];
    FILE *f;
    long n;
    char *b;

    snprintf(p, sizeof(p), "%s/%s", host_dir, rel);
    f = fopen(p, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = calloc(1, n + 1);
    fread(b, 1, n, f);
    fclose(f);
    return b;
}

static u32 get32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

static void edit_crc(unsigned char *b, long *n);

/* Variant of the file MODULES/<name>.OMM: name in the header = name, a patch, CRC recomputed (except edit_crc, which corrupts after it) */
static void variant(const char *name, void (*edit)(unsigned char *b, long *n))
{
    unsigned char *b = malloc(orig_len + 16);
    long n = orig_len;
    u32 crc;
    char rel[64];

    memcpy(b, orig, orig_len);
    memset(b + 8, 0, 8);
    memcpy(b + 8, name, strlen(name));
    if (edit != edit_crc)
        edit(b, &n);
    memset(b + 48, 0, 4);
    crc = crc32_update(0, b, n);
    memcpy(b + 48, &crc, 4);
    if (edit == edit_crc)
        edit(b, &n);
    snprintf(rel, sizeof(rel), "MODULES/%s.OMM", name);
    write_bin(rel, b, n);
    free(b);
}

static void edit_api(unsigned char *b, long *n)
{
    (void)n;
    b[6] = 99;                              /* requires API version 99 */
}

static void edit_reloc(unsigned char *b, long *n)
{
    u32 image = get32(b + 20);

    (void)n;
    memcpy(b + 64 + image, &image, 4);      /* first relocation outside the image */
}

/* An image of 3 bytes with one relocation: the bound of a relocation (image - 4) must not wrap */
static void edit_tiny(unsigned char *b, long *n)
{
    static const unsigned char three[4] = {3, 0, 0, 0}, one[4] = {1, 0, 0, 0}, any[4] = {0x40, 0x42, 0x0f, 0};

    (void)n;
    memcpy(b + 20, three, 4);               /* image */
    memset(b + 28, 0, 4);                   /* entry */
    memcpy(b + 32, one, 4);                 /* relocations */
    memcpy(b + 64 + 3, any, 4);             /* the relocation: 1000000 bytes past the image */
}

static void edit_crc(unsigned char *b, long *n)
{
    (void)n;
    b[200] ^= 1;                            /* after the CRC is computed */
}

static void edit_long(unsigned char *b, long *n)
{
    b[(*n)++] = 0;
}

static void edit_short(unsigned char *b, long *n)
{
    (void)b;
    *n -= 3;
}

/* Instead of module_init: check the image and use the API the way a module does */
static int fake_init(void *entry, const omk_api *api, omk_mod *m)
{
    u32 image = get32(orig + 20), relocs = get32(orig + 32), i, off, n = 0;
    const char *text;
    omk_store *st;
    omk_mem_info mi;
    void *p, *q;
    omk_file *f;

    calls++;
    /* the image has cache lines of its own (MEM_LINES): it starts and ends on a 64-byte boundary, at both addresses */
    CHECK(((uptr)m->image & 63) == 0 && ((uptr)m->exec & 63) == 0);
    CHECK(api->version == OMK_API_VERSION && api->size == sizeof(omk_api));
    CHECK(m->state == MOD_LOADING && !strcmp(m->name, "HELLO"));
    CHECK((u8 *)entry == m->exec + get32(orig + 28));
    CHECK(relocs > 10);
    for (i = 0; i < relocs; i++) {          /* every relocated word = original + execution address */
        off = get32(orig + 64 + image + 4 * i);
        CHECK(get32(m->image + off) == get32(orig + 64 + off) + (u32)(uptr)m->exec);
    }
    for (i = 0; i < m->bss; i++)
        CHECK(m->image[image + i] == 0);

    api->log(m, OMK_LOG_INFO, "fake init, greeting %s", api->cfg_str(m, "greeting", "?"));
    CHECK(api->cfg_int(m, "number", 0) == 42);
    st = api->store_open(m, 64);
    CHECK(st && api->store_open(m, 64) == st);
    CHECK(api->store_set(st, 1, "abc", 3, OMK_STORE_PERSIST) == 0);
    p = api->alloc(m, 500);
    q = api->alloc(m, 100000);              /* larger than the memory = 64K limit */
    CHECK(p && !q);
    api->mem_info(m, &mi);
    CHECK(mi.limit == 64 * 1024 && mi.fails == 1);
    api->free(m, (char *)p + 8);
    api->free(m, p);
    text = api->res(m, 1, 1, &n);
    CHECK(text && n == 24 && !memcmp(text, "Text resource of HELLO", 22));
    CHECK(api->res(m, 1, 2, &n) == 0);
    {
        u8 part[8];

        /* strings: res/strings.txt of HELLO, identical texts share one pointer */
        CHECK(!strcmp(api->str(m, 0), "Hello from OMKit") && !strcmp(api->str(m, 1), "Have a nice shot"));
        CHECK(api->str(m, 2) == api->str(m, 0));
        CHECK(!strcmp(api->str(m, 3), "") && !strcmp(api->str(m, 999), ""));
        /* on-demand: res/disk/2_1.dat, 5000 bytes, byte i = 7i + 3. While module_init runs the data is in memory
         * (m->disk): the module file is not opened for it */
        u32 opens = host_fopens;

        CHECK(m->disk != 0);
        CHECK(api->res(m, 2, 1, &n) == 0 && n == 5000);
        CHECK(api->res_read(m, 2, 1, 4000, part, 8) == 8 && part[0] == 0x63 && part[1] == 0x6a && part[7] == 0x94);
        CHECK(api->res_read(m, 2, 1, 4997, part, 8) == 3 && part[2] == (u8)(4999 * 7 + 3));
        CHECK(api->res_read(m, 2, 1, 5000, part, 8) == 0);
        CHECK(api->res_read(m, 1, 1, 0, part, 8) == 0);    /* in memory: read through res */
        CHECK(host_fopens == opens);
    }
    f = api->file_open(m, "out.txt", "w");
    CHECK(f != 0);
    CHECK(api->file_write(f, "data", 4) == 4);
    api->file_close(f);
    CHECK(api->file_open(m, "../x", "r") == 0);
    CHECK(api->file_open(m, "TOOLONGNAME.TXT", "r") == 0);
    CHECK(api->file_open(m, "A.TXT", "rw") == 0);
    the_api = api;
    CHECK(api->on(m, OMK_EV_PICTMODE_CHECK, OMK_PH_NOTIFY_BEFORE, 0, h_notify_before, 0) == 0);
    CHECK(api->on(m, OMK_EV_PICTMODE_CHECK, OMK_PH_REPLACE, 1, h_replace_lo, 0) == 0);
    CHECK(api->on(m, OMK_EV_PICTMODE_CHECK, OMK_PH_REPLACE, 10, h_replace_hi, 0) == 0);    /* earlier: higher priority */
    CHECK(api->on(m, OMK_EV_PICTMODE_CHECK, OMK_PH_FILTER_AFTER, 0, h_filter_after, 0) == 0);
    CHECK(api->on(m, OMK_EV_PICTMODE_CHECK, OMK_PH_NOTIFY_AFTER, 0, h_notify_after, m) == 0);
    CHECK(api->on(m, OMK_EV_S1_API, OMK_PH_NOTIFY_BEFORE, 0, h_s1, 0) == 0);
    CHECK(api->on(m, OMK_EV_PICTMODE_CHECK, OMK_PH_FILTER_BEFORE, 0, h_filter_after, 0) == -2);
    CHECK(api->on(m, OMK_EV_S1_API, OMK_PH_REPLACE, 0, h_s1, 0) == -2);
    CHECK(api->on(m, 99, OMK_PH_NOTIFY_BEFORE, 0, h_s1, 0) == -1);
    CHECK(api->on(m, OMK_EV_PICTMODE_CHECK, 3, 0, h_s1, 0) == -2);     /* two phases at once */
    CHECK(api->on_power(m, h_power, &power_seen) == 0);
    {
        u32 colors = 0, monos = 0;

        /* picture profiles (API 6): owner, validation, the base picture mode */
        CHECK(api->prof_count(&colors, &monos) == 6 && colors == 3 && monos == 3);
        CHECK(api->prof_set(m, OMK_PROF_COLOR(1)) == -1);               /* not the owner yet */
        CHECK(api->prof_enable(m, h_prof, &prof_seen) == 0 && host_prof_installs == 1);
        CHECK(api->prof_enable(m, h_prof, 0) == -2);
        CHECK(api->prof_set(m, OMK_PROF_COLOR(4)) == -1 && api->prof_set(m, 0x22) == -1);
        host_prof_base = 0;
        CHECK(api->prof_set(m, OMK_PROF_MONO(2)) == -4 && api->prof_get() == OMK_PROF_NONE);
        host_prof_base = 1;
        CHECK(api->prof_set(m, OMK_PROF_MONO(2)) == 0 && api->prof_get() == OMK_PROF_MONO(2));
    }
    {
        /* pictures of profiles (API 8): sizes, the owner only, mode icons of full width, labels up to it */
        extern u32 host_icons;
        static const u8 px[(124 + 1) / 2 * 60];
        u32 w = 0, h = 0, before = host_icons;

        CHECK(api->prof_picture_size(OMK_PIC_MODE(OMK_PROF_COLOR(1), OMK_PROF_ICON_NORMAL), &w, &h) == 0 && w == 66 && h == 50);
        CHECK(api->prof_picture_size(OMK_PIC_LABEL(OMK_PROF_MONO(2), 1), &w, &h) == 0 && w == 124 && h == 39);
        CHECK(api->prof_picture_size(0x777, &w, &h) == -1);
        CHECK(api->prof_picture_size(OMK_PIC_PANEL(OMK_PROF_COLOR(3), 0), &w, &h) == 0 && w == 128 && h == 39);
        {
            static const s16 mx[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
            extern u32 host_matrices;

            CHECK(api->prof_mono_matrix(m, 3, 2, mx) == 0 && host_matrices == 1);
            CHECK(api->prof_mono_matrix(m, 9, 2, mx) == -1 && api->prof_mono_matrix(m, 3, 4, mx) == -1);
            CHECK(api->prof_mono_matrix(m, 0, 1, mx) == -1 && api->prof_mono_matrix(m, 3, 2, 0) == 0);
        }
        CHECK(api->prof_picture_size(OMK_PIC_SHOOT(OMK_PROF_MONO(1), 1), &w, &h) == 0 && w == 80 && h == 36);
        CHECK(api->prof_picture(m, OMK_PIC_MODE(OMK_PROF_MONO(1), OMK_PROF_ICON_LARGE), 80, 60, px) == 0);
        CHECK(api->prof_picture(m, OMK_PIC_LABEL(OMK_PROF_COLOR(1), 0), 107, 39, px) == 0);
        CHECK(api->prof_picture(m, OMK_PIC_LABEL(OMK_PROF_COLOR(1), 0), 125, 39, px) == -1);     /* too wide */
        CHECK(api->prof_picture(m, OMK_PIC_LABEL(OMK_PROF_COLOR(1), 0), 107, 40, px) == -1);     /* wrong height */
        CHECK(api->prof_picture(m, 0x777, 66, 50, px) == -1);
        CHECK(api->prof_picture(module_at(1), OMK_PIC_MODE(OMK_PROF_COLOR(1), 0), 66, 50, px) == -1 || module_at(1) == m);
        CHECK(host_icons == before + 2);
    }
    /* the ISP service: not available on the host unless the test says so (test_fail: taken, then released) */
    CHECK(api->isp_scope(m, OMK_ISP_STILLS) == (host_isp_ok ? 0 : -3));
    if (off_in_init)
        power_hook(POWER_HOOK_OFF, 1);      /* the lever, while the module was starting */
    return fail_rc;
}

/* Event calls after loading: phase and priority order, the next chain, replacement without next, nested call, defer */
static void test_events(void)
{
    omk_ev_pictmode_check d;
    char *t;

    CHECK(host_installs[OMK_EV_PICTMODE_CHECK] == 1 && host_installs[OMK_EV_S1_API] == 0);   /* s1 is disabled in the INI */
    CHECK(ev_register(module_at(0), OMK_EV_S1_API, OMK_PH_NOTIFY_BEFORE, 0, h_s1, 0) == -3);

    d.mode = 7;
    d.allowed = 1;
    ntrace = 0;
    CHECK(ev_dispatch(OMK_EV_PICTMODE_CHECK, &d, orig_pm, 0) == 1);
    CHECK(!strcmp(trace, "nRrOfa"));
    CHECK(d.mode == 7 && d.allowed == 0 && after_allowed == 0 && orig_calls == 1);

    d.mode = 5;
    d.allowed = 1;
    ntrace = 0;
    CHECK(ev_dispatch(OMK_EV_PICTMODE_CHECK, &d, orig_pm, 0) == 0);
    CHECK(!strcmp(trace, "nRfa") && d.allowed == 0 && orig_calls == 1);

    d.mode = 9;
    d.allowed = 0;
    ntrace = 0;
    CHECK(ev_dispatch(OMK_EV_PICTMODE_CHECK, &d, orig_pm, 0) == 1);
    CHECK(!strcmp(trace, "nROrOfa") && d.allowed == 1 && orig_calls == 3);

    ntrace = 0;
    CHECK(ev_dispatch(OMK_EV_S1_API, 0, orig_pm, 0) == 1);
    CHECK(!strcmp(trace, "O"));             /* the s1 handler is disabled in the INI: the original only */

    CHECK(deferred == 0);
    CHECK(host_wakes == 3);                 /* every defer woke the core task */
    ev_service(100000);
    CHECK(deferred == 9);                   /* three calls of 3 each */

    /* a call from another task while the handlers run: the original only, counted apart from the nested ones; the
     * event is free again afterwards (the next call gets its handlers) */
    d.mode = 11;
    d.allowed = 0;
    ntrace = 0;
    CHECK(ev_dispatch(OMK_EV_PICTMODE_CHECK, &d, orig_pm, 0) == 1);
    CHECK(!strcmp(trace, "nROrOfa") && d.allowed == 1 && orig_calls == 6);
    d.mode = 7;
    ntrace = 0;
    CHECK(ev_dispatch(OMK_EV_PICTMODE_CHECK, &d, orig_pm, 0) == 1 && !strcmp(trace, "nRrOfa"));
    ev_service(200000);
    CHECK(deferred == 15);

    /* a deferred call that defers itself runs once a pass: the pass ends, the core loop goes on */
    CHECK(ev_defer(module_at(0), again, module_at(0)) == 0);
    ev_service(200001);
    CHECK(again_runs == 1);
    CHECK(ev_defer(module_at(0), job, (void *)1) == 0);     /* queued behind the call's own repeat: still this pass */
    ev_service(200002);
    CHECK(again_runs == 2 && deferred == 16);
    again_stop = 1;
    ev_service(200003);
    ev_service(200004);
    CHECK(again_runs == 3);
    ev_service(300000);                     /* the counters' line (every 10 s) */
    log_flush();
    t = read_text("LOG/CORE.LOG");
    CHECK(t && strstr(t, "event pictmode_check: 1. HELLO replace priority 10"));
    CHECK(t && strstr(t, "event pictmode_check: 2. HELLO replace priority 1"));
    CHECK(t && strstr(t, "event pictmode_check: hook installed, 5 handlers"));
    CHECK(t && strstr(t, "event s1_api: HELLO handler off in OMKIT.INI"));
    CHECK_DEBUG(t && strstr(t, "event pictmode_check: 3 calls, 0 without handlers on another cpu, 1 nested"));
    CHECK_DEBUG(t && strstr(t, "event pictmode_check: 5 calls, 0 without handlers on another cpu, 1 nested"));
    CHECK(t && strstr(t, " W event pictmode_check: 1 calls came from another task while the handlers were running"));
    CHECK(t && !strstr(t, "defer queue full"));
    CHECK(t && strstr(t, "phase 3 not allowed"));
    free(t);
}

static int count(const char *t, const char *what)
{
    int n = 0;

    while ((t = strstr(t, what)) != 0) {
        n++;
        t++;
    }
    return n;
}

static void write_ini(const char *text)
{
    char p[512];
    FILE *f;

    snprintf(p, sizeof(p), "%s/OMKIT.INI", host_dir);
    f = fopen(p, "wb");
    fputs(text, f);
    fclose(f);
}

static void prepare(void)
{
    char p[512];
    unsigned char *b;

    snprintf(p, sizeof(p), "%s/MODULES", host_dir);
    mkdir(p, 0777);
    snprintf(p, sizeof(p), "%s/HELLO", host_dir);
    mkdir(p, 0777);
    write_bin("MODULES/HELLO.OMM", orig, orig_len);
    write_bin("MODULES/COPY.OMM", orig, orig_len);          /* the name in the file is HELLO */
    variant("NEWAPI", edit_api);
    variant("OTHER", edit_reloc);
    variant("TINY", edit_tiny);
    variant("LONG", edit_long);
    variant("SHORT", edit_short);
    variant("CRC", edit_crc);
    b = malloc(orig_len);
    memcpy(b, orig, orig_len);
    b[0] ^= 1;
    write_bin("MODULES/MAGIC.OMM", b, 64);
    free(b);
}

/* Normal load: one working module and every kind of failure */
static void test_load(void)
{
    char *t;
    u32 total, used, largest;
    omk_mem_info mi;

    write_ini("[modules]\n"
              "MAGIC = on\nNEWAPI = on\nCOPY = on\nLONG = on\nSHORT = on\nCRC = on\nMISSING = on\nCORE = on\nOTHER = on\n"
              "TINY = on\n"
              "bad.name = on\nSKIPPED = off\nHELLO = on\nHELLO = on\n"
              "[HELLO]\ngreeting = test\nnumber = 42\nmemory = 64K\nevent_s1_api = off\n");
    CHECK(cfg_load() == 1);
    CHECK(modules_load() == 1);
    CHECK(calls == 1);
    CHECK(modules_count() == 1 && module_at(0)->state == MOD_RUNNING);
    {
        /* whole lines for the image: 64 in front of it is not its own block's data, the block ends on a boundary */
        omk_mod *m = module_at(0);
        omk_mem_info mi;
        u32 lines = (m->image_size + m->bss + 63) / 64 * 64;

        mem_info(m->owner, &mi);
        CHECK(lines % 64 == 0 && (((uptr)m->image + lines) & 63) == 0 && mi.used >= lines + 16);
    }
    log_flush();
    t = read_text("LOG/CORE.LOG");
    CHECK(t && strstr(t, "module MAGIC: not an OMKit module"));
    CHECK(t && strstr(t, "module NEWAPI: needs a newer core API"));
    CHECK(t && strstr(t, "module COPY: name in the file differs"));
    CHECK(t && strstr(t, "module LONG: file longer than the header says"));
    CHECK(t && strstr(t, "module SHORT: file ends inside"));
    CHECK(t && strstr(t, "module CRC: checksum mismatch"));
    CHECK(t && strstr(t, "module MISSING: no file MODULES"));
    CHECK(t && strstr(t, "module CORE: name must be"));
    CHECK(t && strstr(t, "module bad.name: name must be"));
    CHECK(t && strstr(t, "module OTHER: relocation outside the image"));
    CHECK(t && strstr(t, "module TINY: sizes in the header are out of range"));
    CHECK(t && !strstr(t, "SKIPPED"));
    CHECK(t && count(t, "module HELLO 1.0:") == 1);
    CHECK(t && strstr(t, " I HELLO: fake init, greeting test"));
    CHECK(t && strstr(t, " W memory: HELLO wants 100000 bytes, over its limit"));
    CHECK(t && strstr(t, " E memory: HELLO frees "));
    CHECK_DEBUG(t && strstr(t, "TOOLONGNAME.TXT"));
    CHECK(t && strstr(t, "modules: 1 running, 0 failed"));
    free(t);
    t = read_text("HELLO/OUT.TXT");
    CHECK(t && !strcmp(t, "data"));
    free(t);
    /* rejected modules have their memory freed: only HELLO is left in the arena */
    mem_arena(&total, &used, &largest);
    mem_info(module_at(0)->owner, &mi);
    CHECK(mi.used && used == mi.used);
    {
        /* after module_init the copy of the on-demand data is gone (the arena check above): the same bytes come from
         * the module file, opened for the read */
        const omk_api *api = omk_api_get();
        omk_mod *m = module_at(0);
        u32 opens = host_fopens;
        u8 part[8];

        CHECK(m->disk == 0);
        CHECK(api->res_read(m, 2, 1, 4000, part, 8) == 8 && part[0] == 0x63 && part[1] == 0x6a && part[7] == 0x94);
        CHECK(api->res_read(m, 2, 1, 4997, part, 8) == 3 && part[2] == (u8)(4999 * 7 + 3));
        CHECK(host_fopens == opens + 2);
    }
    test_events();

    /* power states (API 5): the handler gets every event with its ctx; registering after module_init is refused */
    modules_power(OMK_POWER_OFF);
    modules_power(OMK_POWER_RESUME);
    modules_power(OMK_POWER_SLEEP);
    modules_power(OMK_POWER_WAKE);
    CHECK(power_seen[OMK_POWER_OFF] == 1 && power_seen[OMK_POWER_RESUME] == 1 && power_seen[OMK_POWER_SLEEP] == 1 &&
          power_seen[OMK_POWER_WAKE] == 1 && power_ctx_ok == 4);
    CHECK(omk_api_get()->on_power(module_at(0), h_power, 0) == -3);

    /* the user picks a profile in the UI: the owner hears it in the core task, once per change */
    CHECK(omk_api_get()->prof_enable(module_at(0), h_prof, 0) == -3);
    host_prof_ui = OMK_PROF_COLOR(3);
    host_prof_seq++;
    prof_service(1000);
    prof_service(1100);
    CHECK(prof_calls == 1 && prof_seen == OMK_PROF_COLOR(3));
    host_prof_ui = OMK_PROF_NONE;
    host_prof_seq++;
    prof_service(1200);
    CHECK(prof_calls == 2 && prof_seen == OMK_PROF_NONE);
    log_flush();
    t = read_text("LOG/CORE.LOG");
    CHECK_DEBUG(t && strstr(t, "power: HELLO power-off took ") && strstr(t, "power: HELLO wake took "));
    CHECK(t && strstr(t, "HELLO: on_power only from module_init"));
    CHECK(t && strstr(t, "profiles: 3 colour and 3 monochrome, owned by HELLO; stored picture mode 1"));
    CHECK(t && strstr(t, "profiles: the stored picture mode (5) is not the base one, monochrome profile 2 not applied"));
    CHECK(t && strstr(t, "profiles: the user chose colour 3"));
    free(t);
}

/* module_init returned an error: the module is unloaded, the store closed, the image stays allocated */
static void test_fail(void)
{
    char *t;
    u32 total, used, largest, image;

    fail_rc = 5;
    host_isp_ok = 1;                        /* the module takes the ISP service before it fails ... */
    host_isp_busy = 1;                      /* ... and the driver refuses the first "back to stock" as too soon */
    write_ini("[modules]\nHELLO = on\n[HELLO]\nnumber = 42\nmemory = 64K\n");
    CHECK(cfg_load() == 1);
    CHECK(modules_load() == 1);
    CHECK(calls == 1 && modules_count() == 1 && module_at(0)->state == MOD_FAILED);
    CHECK(host_isp_stock == 1);             /* the ISP service released: stock tables asked for, refused (-2) ... */
    isp_service(1000);
    CHECK(host_isp_stock == 2);             /* ... asked again from the core loop, taken ... */
    isp_service(2000);
    CHECK(host_isp_stock == 2);             /* ... and not again */
    modules_power(OMK_POWER_OFF);           /* the failed module registered a handler: it was dropped */
    CHECK(power_seen[OMK_POWER_OFF] == 0);
    CHECK(host_prof == OMK_PROF_NONE);      /* it owned the profiles: released, the profile it set is off */
    store_service(100000);
    log_flush();
    t = read_text("LOG/CORE.LOG");
    CHECK(t && strstr(t, "module HELLO: module_init returned 5, unloaded"));
    CHECK(t && strstr(t, "isp: HELLO failed to start, the service is free"));
    CHECK(t && strstr(t, "modules: 0 running, 1 failed"));
    free(t);
    t = read_text("STORE/HELLO.STA");
    CHECK(!t);
    free(t);
    image = (get32(orig + 20) + get32(orig + 24) + 63) / 64 * 64 + 16;    /* whole 64-byte lines and the block header */
    mem_arena(&total, &used, &largest);
    CHECK(used == image);
}

/* A power-off begins while the first module starts: the rest is not read until the camera runs again */
static void test_power(void)
{
    char *t;
    u32 now = 1000;

    write_ini("[modules]\nHELLO = on\nCRC = on\n[HELLO]\ngreeting = test\nnumber = 42\nmemory = 64K\n");
    CHECK(cfg_load() == 1);
    CHECK(power_service(now) == 1);
    off_in_init = 1;
    CHECK(modules_load() == 0);
    off_in_init = 0;
    CHECK(calls == 1 && modules_count() == 1 && module_at(0)->state == MOD_RUNNING);
    CHECK(host_installs[OMK_EV_PICTMODE_CHECK] == 0);       /* the event hooks wait for the rest */
    log_flush();
    t = read_text("LOG/CORE.LOG");
    CHECK(t && strstr(t, "modules: a power-off or a sleep began, the rest loads when the camera runs again"));
    CHECK(t && !strstr(t, "module CRC") && !strstr(t, "modules: 1 running"));
    free(t);
    CHECK(power_service(now) == 0);         /* the core loop acknowledges the power-off ... */
    CHECK(power_seen[OMK_POWER_OFF] == 1);  /* ... and the module that did start hears of it */
    now += POWER_CANCEL_MS + 1;
    power_service(now);                     /* ... which does not happen */
    CHECK(power_service(now) == 1 && power_seen[OMK_POWER_RESUME] == 1);
    CHECK(modules_load() == 1);
    CHECK(calls == 1 && modules_count() == 1);              /* HELLO is not started twice */
    CHECK(host_installs[OMK_EV_PICTMODE_CHECK] == 1);
    log_flush();
    t = read_text("LOG/CORE.LOG");
    CHECK(t && strstr(t, "module CRC: checksum mismatch") && strstr(t, "modules: 1 running, 0 failed"));
    CHECK(t && count(t, "module HELLO 1.0:") == 1);
    free(t);
}

int main(int argc, char **argv)
{
    FILE *f;

    if (argc < 4)
        return 2;
    host_dir = argv[1];
    f = fopen(argv[2], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    orig_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    orig = malloc(orig_len);
    fread(orig, 1, orig_len, f);
    fclose(f);
    prepare();
    mem_init();
    log_setup(1, OMK_LOG_DEBUG);
    module_call = fake_init;
    if (!strcmp(argv[3], "load"))
        test_load();
    else if (!strcmp(argv[3], "power"))
        test_power();
    else
        test_fail();
    if (failed)
        fprintf(stderr, "%d checks failed\n", failed);
    else
        printf("ok\n");
    return failed != 0;
}

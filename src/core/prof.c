/* Picture profiles service (omk_prof.h). The driver shows the active profile to the UI and applies it to shooting;
 * here: who owns the service, the user's choices delivered to the owner in the core task,
 * upkeep of the driver hooks, counters in the log. */
#include "omk_core.h"
#include "omk_log.h"
#include "omk_mod.h"
#include "omk_prof.h"

#define UPKEEP_MS 1000
#define STATS_MS 10000

static omk_mod *owner;
static omk_prof_fn owner_fn;
static void *owner_ctx;
static u32 seen_seq, upkeep_ms, stats_ms, warned, seen_takes, lv_ms;
static drv_lv_view lv_seen;
static int lv_bad, lv_logged;

static int same_bytes(const void *a, const void *b, u32 n)
{
    const u8 *x = a, *y = b;

    while (n--)
        if (*x++ != *y++)
            return 0;
    return 1;
}

/* Debug: the live view context, a line when it changes. Once a second */
static void watch_live_view(u32 now)
{
    drv_lv_view v;

    if (log_level() < OMK_LOG_DEBUG || now - lv_ms < 1000)
        return;
    lv_ms = now;
    mem_set(&v, 0, sizeof(v));
    if (drv_prof_lv_read(&v)) {
        if (!lv_bad)
            log_printf(OMK_LOG_DEBUG, "profiles: lv bad slot %u/%u", v.active, v.last);
        lv_bad = 1;
        return;
    }
    lv_bad = 0;
    if (lv_logged && same_bytes(&v, &lv_seen, sizeof(v)))
        return;
    lv_logged = 1;
    mem_copy(&lv_seen, &v, sizeof(v));
    log_printf(OMK_LOG_DEBUG, "profiles: lv slot %u/%u gen %x mode %u.%u desc %u flt %u,%u cc %u mc %u lvl/sh/gr %u,%u,%u", v.active,
               v.last, v.gen, v.mode, v.mode2, v.desc, v.filter, v.toning, v.cc, v.mc, v.level, v.shading, v.grain);
    log_printf(OMK_LOG_DEBUG, "profiles: lv sat/sh/gr/ct %u,%u,%u,%u s12 %u,%u tone %u,%u,%u (active profile %x)", v.sat, v.sharp,
               v.grad, v.contrast, v.s12[0], v.s12[1], v.tone[0], v.tone[1], v.tone[2], drv_prof_get());
}
static drv_prof_info logged;
static u32 ed_logged[2], ed_ms;           /* graphic editor counters last logged */
static u32 imp_logged[4];                   /* import counters last logged */

static int valid(u32 prof)
{
    u32 colors, monos, n = OMK_PROF_NUM(prof);

    if (prof == OMK_PROF_NONE)
        return 1;
    if (!drv_prof_count(&colors, &monos) || (prof & ~0x1fu) || !n)
        return 0;
    return n <= (OMK_PROF_IS_MONO(prof) ? monos : colors);
}

static const char *kind(u32 prof)
{
    return prof == OMK_PROF_NONE ? "stock" : OMK_PROF_IS_MONO(prof) ? "monochrome" : "colour";
}

int prof_count(u32 *colors, u32 *monos)
{
    u32 c = 0, m = 0;

    if (!drv_prof_count(&c, &m))
        c = m = 0;
    if (colors)
        *colors = c;
    if (monos)
        *monos = m;
    return c + m;
}

int prof_enable(omk_mod *m, omk_prof_fn fn, void *ctx)
{
    u32 c, mo;
    int rc;

    if (!m || m->state != MOD_LOADING)
        return -3;
    if (owner)
        return -2;
    if (!drv_prof_count(&c, &mo))
        return -1;
    /* On the card before and after: if the hooks take the camera down, the log shows how far it got (core 0.62 crashed
     * here before anything reached the card) */
    log_printf(OMK_LOG_INFO, "profiles: installing hooks");
    log_flush();
    rc = drv_prof_install(cfg_bool("core", "fw_cache_inv", 0));
    if (rc) {
        log_printf(OMK_LOG_ERROR, "profiles: hooks not installed, code %d", rc);
        return -1;
    }
    owner = m;
    owner_fn = fn;
    owner_ctx = ctx;
    drv_prof_stats(&logged);
    seen_seq = logged.ui_seq;
    log_printf(OMK_LOG_INFO, "profiles: %u colour and %u monochrome, owned by %s; stored picture mode %u", c, mo, m->name,
               drv_prof_stored_mode());
    log_flush();
    return 0;
}

int prof_set(omk_mod *m, u32 prof)
{
    if (!m || m != owner || !valid(prof))
        return -1;
    if (prof != OMK_PROF_NONE && !drv_prof_base()) {
        log_printf(OMK_LOG_WARN, "profiles: the stored picture mode (%u) is not the base one, %s profile %u not applied",
                   drv_prof_stored_mode(), kind(prof), OMK_PROF_NUM(prof));
        return -4;
    }
    drv_prof_set(prof);
    log_printf(OMK_LOG_INFO, "profiles: %s profile %u set by %s", kind(prof), OMK_PROF_NUM(prof), m->name);
    return 0;
}

int prof_owned(void)
{
    return owner != 0;
}

u32 prof_get(void)
{
    return drv_prof_get();
}

int prof_mono_matrix(omk_mod *m, u32 filter, u32 level, const s16 *matrix)
{
    int rc;

    if (!m || m != owner || filter < 1 || filter > 8 || level < 1 || level > 3)
        return -1;
    rc = drv_prof_mono_matrix(filter, level, matrix);
    if (rc)
        log_printf(OMK_LOG_WARN, "profiles: monochrome filter %u level %u matrix not set, code %d", filter, level, rc);
    return rc;
}

int prof_mono_matrix_get(u32 filter, u32 level, s16 *matrix)
{
    int rc;

    if (!matrix || filter < 1 || filter > 8 || level < 1 || level > 3)
        return -1;
    rc = drv_prof_mono_matrix_get(filter, level, matrix);
    if (rc)
        log_printf(OMK_LOG_WARN, "profiles: monochrome filter %u level %u matrix not readable, code %d", filter, level, rc);
    return rc;
}

int prof_picture_size(u32 pic, u32 *w, u32 *h)
{
    if (!w || !h)
        return -1;
    return drv_prof_picture_size(pic, w, h);
}

int prof_picture(omk_mod *m, u32 pic, u32 w, u32 h, const u8 *pixels)
{
    u32 ww, hh;
    int rc;

    if (!m || m != owner || !pixels || prof_picture_size(pic, &ww, &hh) || !w || w > ww || h != hh)
        return -1;
    rc = drv_prof_picture(pic, w, h, pixels);
    if (rc)
        log_printf(OMK_LOG_WARN, "profiles: picture %x not set, code %d", pic, rc);
    return rc;
}

void prof_drop_module(omk_mod *m)
{
    if (!owner || owner != m)
        return;
    drv_prof_set(OMK_PROF_NONE);
    owner = 0;
    owner_fn = 0;
    log_printf(OMK_LOG_WARN, "profiles: %s failed to start, profiles stay in the list without an owner", m->name);
}

void prof_service(u32 now)
{
    drv_prof_info s;
    u32 gmin, glast, gcount;
    int rc;

    drv_prof_flush();                       /* filter matrices set since the last loop: one cache push for them all */
    if (!owner)
        return;
    drv_prof_prepare();                     /* the block the shooting hook copies: here, in the core task */
    drv_edp_service();                      /* the graphic editor's drawing off outside its item */
    watch_live_view(now);
    drv_prof_stats(&s);
    if (s.takes != seen_takes) {            /* a picture: what happened to the profile */
        seen_takes = s.takes;
        log_printf(OMK_LOG_DEBUG, "profiles: shot %u, active %x, applied %u, skipped: mode %u, movie/scene %u, art bracketing %u, "
                   "block %u, allocator %u (mode %u)", s.takes, drv_prof_get(), s.applied, s.skip_mode, s.skip_cam, s.skip_bkt,
                   s.skip_block, s.skip_alloc, s.alloc_mode);
    }
    if (s.ui_seq != seen_seq) {             /* the user picked a profile or left it: tell the owner, here in the core task */
        seen_seq = s.ui_seq;
        log_printf(OMK_LOG_INFO, "profiles: the user chose %s %u", kind(s.ui_prof), OMK_PROF_NUM(s.ui_prof));
        if (owner_fn && owner->state == MOD_RUNNING)
            owner_fn(s.ui_prof, owner_ctx);
    }
    if (now - upkeep_ms >= UPKEEP_MS) {
        upkeep_ms = now;
        rc = drv_prof_service();
        if (rc == 2)
            log_printf(OMK_LOG_INFO, "profiles: the firmware loaded its picture table again, icons put back");
        else if (rc > 0)
            log_printf(OMK_LOG_INFO, "profiles: the firmware restored its tables, hooks put back");
        else if (rc < 0 && !warned) {
            warned = 1;
            log_printf(OMK_LOG_WARN, "profiles: foreign values in the firmware tables, left alone");
        }
    }
    if (now - stats_ms >= STATS_MS && (s.takes != logged.takes || s.reads != logged.reads || s.writes != logged.writes ||
                                       s.lv_reads != logged.lv_reads || s.lv_applied != logged.lv_applied ||
                                       s.recalcs != logged.recalcs)) {
        stats_ms = now;
        log_printf(OMK_LOG_DEBUG, "profiles: active %x, ui reads %u writes %u, rebuilds %u, allocator: substituted %u mode %u; "
                   "calls on other CPUs %u", drv_prof_get(), s.reads, s.writes, s.recalcs, s.alloc_sub, s.alloc_mode, s.other_cpu);
        log_printf(OMK_LOG_DEBUG, "profiles: live view mode %u, raw 111 substituted %u, context copies %u", s.lv_mode, s.lv_reads,
                   s.lv_applied);
        mem_copy(&logged, &s, sizeof(s));
    }
    {
        u32 lines, selects, skipped, last, bad;

        drv_imp_stats(&lines, &selects, &skipped, &last, &bad);
        if (bad != imp_logged[3]) {
            imp_logged[3] = bad;
            log_printf(OMK_LOG_WARN, "profiles: loading from outside: setting names not as expected (component 0x106), "
                       "%u profile lines refused", bad);
        }
        if (lines != imp_logged[0] || selects != imp_logged[1] || skipped != imp_logged[2]) {   /* a load from outside */
            imp_logged[0] = lines;
            imp_logged[1] = selects;
            imp_logged[2] = skipped;
            log_printf(OMK_LOG_INFO, "profiles: loaded from outside: %u cell lines, %u profile choices (last %s %u), "
                       "%u lines for custom modes skipped", lines, selects, kind(last), OMK_PROF_NUM(last), skipped);
        }
    }
    if ((s.ed_items != ed_logged[0] || s.ed_renders != ed_logged[1]) && now - ed_ms >= 1000) {   /* only while an editor
                                                                                                     item is in use */
        ed_ms = now;
        ed_logged[0] = s.ed_items;
        ed_logged[1] = s.ed_renders;
        log_printf(OMK_LOG_DEBUG, "profiles: editor: items %u, Color Creator renderer %u (CPU %u), drawn %u, "
                   "faults %u", s.ed_items, s.ed_renders, s.ed_cpu, s.ed_drawn, s.ed_failed);
        drv_edp_gaps(&gmin, &glast, &gcount);
        if (gcount)
            log_printf(OMK_LOG_DEBUG, "profiles: editor: film look frames %u apart at least, last %u (ms, %u)",
                       gmin / 1000, glast / 1000, gcount);
    }
}

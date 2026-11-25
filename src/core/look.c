/* Film look service (omk_look.h). The driver shows the look in the slot of the picture mode
 * screen and steps its entries with the dials; here: who owns the service, the user's choices delivered to the owner in the
 * core task, counters in the log. The colour itself is the owner's business (the ISP service). */
#include "omk_core.h"
#include "omk_log.h"
#include "omk_mod.h"
#include "omk_look.h"
#include "omk_prof.h"

static omk_mod *owner;
static omk_look_fn owner_fn;
static void *owner_ctx;
static drv_look_info seen;
static u32 trace_at;                        /* the dial trace: the next entry to log */
static u32 upkeep_ms, warned, seen_takes;
static int edits_rc, grain_rc, optics_rc;
static u8 optics_val[OMK_OPTICS_ROWS];      /* the optics rows' strengths of the owner's chosen entry (API 20) */
static u32 optics_entry;
static int optics_known;

int look_enable(omk_mod *m, const char *slot, omk_look_fn fn, void *ctx)
{
    int rc;

    if (!m || m->state != MOD_LOADING)
        return -3;
    if (owner)
        return -2;
    if (!slot)
        slot = "";
    log_printf(OMK_LOG_INFO, "look: installing hooks, slot %s", slot);
    log_flush();
    rc = drv_look_install(cfg_bool("core", "fw_cache_inv", 0), slot);
    if (rc == -1) {
        log_printf(OMK_LOG_ERROR, "look: no slot '%s' here; slots: %s", slot, drv_look_slots());
        return -1;
    }
    if (rc) {
        log_printf(OMK_LOG_ERROR, "look: hooks not installed, code %d", rc);
        return -1;
    }
    owner = m;
    owner_fn = fn;
    owner_ctx = ctx;
    drv_look_stats(&seen);
    log_printf(OMK_LOG_INFO, "look: owned by %s, in the place of picture mode %u (%s); stored picture mode %u", m->name,
               drv_look_mode(), slot, drv_prof_stored_mode());
    log_flush();
    return 0;
}

int look_list(omk_mod *m, const char *const *names, u32 count, u32 current)
{
    int rc;

    if (!m || m != owner || count > OMK_LOOK_MAX)
        return -1;
    rc = drv_look_list(names, count, current);
    if (rc)
        log_printf(OMK_LOG_WARN, "look: %u entries not set, code %d", count, rc);
    else
        log_printf(OMK_LOG_DEBUG, "look: %u entries, entry %u chosen", count, count ? current % count : 0);
    drv_look_stats(&seen);                  /* the module set the entry itself: no event for it */
    return rc;
}

int look_set(omk_mod *m, int on)
{
    if (!m || m != owner)
        return -1;
    if (on && !drv_prof_base()) {
        log_printf(OMK_LOG_WARN, "look: the stored picture mode (%u) is not Natural, the look is not turned on",
                   drv_prof_stored_mode());
        return -4;
    }
    drv_look_set(on);
    drv_look_stats(&seen);
    log_printf(OMK_LOG_INFO, "look: %s by %s", on ? "on" : "off", m->name);
    return 0;
}

int look_get(void)
{
    return owner && drv_look_active();
}

int look_picture_size(u32 pic, u32 *w, u32 *h)
{
    if (!w || !h)
        return -1;
    return drv_look_picture_size(pic, w, h);
}

int look_picture(omk_mod *m, u32 pic, u32 w, u32 h, const u8 *pixels)
{
    u32 ww, hh;
    int rc;

    if (!m || m != owner || !pixels || look_picture_size(pic, &ww, &hh) || !w || w > ww || h != hh)
        return -1;
    rc = drv_look_picture(pic, w, h, pixels);
    if (rc)
        log_printf(OMK_LOG_WARN, "look: picture %u not set, code %d", pic, rc);
    return rc;
}

int look_exif(omk_mod *m, const char *text)
{
    int rc = drv_exif_text(text);

    if (rc)
        log_printf(OMK_LOG_WARN, "%s: exif_comment: no shooting hook (neither profiles nor the film look in place)",
                   m ? m->name : "?");
    else
        log_printf(OMK_LOG_DEBUG, "%s: EXIF UserComment: %s", m ? m->name : "?", text ? text : "the camera's own");
    return rc;
}

int look_name(omk_mod *m, const char *text)
{
    int rc;

    if (!m || m != owner)
        return -1;
    rc = drv_look_name(text);
    if (rc)
        log_printf(OMK_LOG_WARN, "look: name not set, code %d", rc);
    else
        log_printf(OMK_LOG_DEBUG, "look: the slot is named '%s'", text);
    return rc;
}

int look_edits(omk_mod *m, const omk_look_edits *e)
{
    int rc;

    if (!m || m != owner || !e)
        return -1;
    rc = drv_look_edits(e);
    if (rc && rc != edits_rc)               /* once per code: the module calls it at every change */
        log_printf(OMK_LOG_WARN, "look: editors not set, code %d", rc);
    edits_rc = rc;
    return rc;
}

/* API 17: the tile and strength of the look's grain for entry `entry` (grain.c) */
int look_grain(omk_mod *m, u32 entry, s32 tile, u32 level)
{
    int rc;

    if (!m || m != owner)
        return -1;
    rc = grain_look(entry, tile, level);
    if (rc && rc != grain_rc)               /* once per code */
        log_printf(OMK_LOG_WARN, "look: grain not set, code %d", rc);
    grain_rc = rc;
    return rc;
}

/* API 20: the optics rows' strengths of entry `entry`: the driver shows them while the UI is at that entry; core keeps
 * them as the chosen entry's for the look's stills (look_optics_now) whatever the driver answers - they are the owner's
 * values of the preset it applies, the rows only show them */
int look_optics(omk_mod *m, u32 entry, const u8 *strength, u32 count)
{
    u32 i, same;
    int rc;

    if (!m || m != owner || !strength || count != OMK_OPTICS_ROWS)
        return -1;
    for (i = 0, same = optics_known && entry == optics_entry; i < OMK_OPTICS_ROWS; i++) {
        if (strength[i] > OMK_OPTICS_MAX)
            return -1;
        same &= strength[i] == optics_val[i];
    }
    rc = drv_look_optics(entry, strength);
    if (rc && rc != optics_rc)              /* once per code */
        log_printf(rc == -3 ? OMK_LOG_INFO : OMK_LOG_WARN, rc == -3 ? "look: optics rows off (no optics layer: [optics] "
                   "layer)" : "look: optics rows not shown, code %d", rc);
    optics_rc = rc;
    if (!same)
        log_printf(OMK_LOG_DEBUG, "look: entry %u: bloom %u, halation %u", entry + 1, strength[OMK_OPTICS_BLOOM],
                   strength[OMK_OPTICS_HALATION]);
    for (i = 0; i < OMK_OPTICS_ROWS; i++)
        optics_val[i] = strength[i];
    optics_entry = entry;
    optics_known = 1;
    return rc;
}

int look_optics_now(u8 *out)
{
    u32 i;
    int on = owner && optics_known && drv_look_active();

    for (i = 0; i < OMK_OPTICS_ROWS; i++)
        out[i] = on ? optics_val[i] : 0;
    return on;
}

/* CANI v1 (OMKit/docs/formats/isp.md): a 64-byte header {u16 version 1, u16 kind 1 (a drawing), u16 256, u16 256, u16
 * records 1..64, u16 flags, u32 bytes of records <= 6144, texts}, then the records {u8 op 1 fill non-zero, 2 fill even-odd,
 * 3 stroke; u8 caps 0..2; u16 0; u32 RGBA; u32 stroke width (float bits); u16 segments 1..352; u16 points 0..320} + the
 * segments (OpenVG absolute MOVE_TO 2, LINE_TO 4, QUAD_TO 10, CUBIC_TO 12, CLOSE 0; the first a MOVE_TO) padded to 4 +
 * the points (s16 x, y in quarter pixels, 0..1024). Everything is checked here, once: the renderer on CPU 1 trusts it */
#define CANI_HEAD 64
#define CANI_RECORDS 64
#define CANI_BYTES 6144
#define CANI_SEG 352
#define CANI_PTS 320

static u32 rd16(const u8 *p)
{
    return p[0] | p[1] << 8;
}

static u32 rd32(const u8 *p)
{
    return rd16(p) | rd16(p + 2) << 16;
}

const char *cani_check(const u8 *b, u32 n)
{
    u32 count, size, o, r, i, op, width, nseg, npts, pts, s;

    if (n < CANI_HEAD)
        return "shorter than its header";
    if (rd16(b) != 1)
        return "not version 1";
    if (rd16(b + 2) != 1)
        return "not a drawing (kind 1)";
    if (rd16(b + 4) != 256 || rd16(b + 6) != 256)
        return "not for a 256 x 256 surface";
    count = rd16(b + 8);
    size = rd32(b + 12);
    if (!count || count > CANI_RECORDS || size > CANI_BYTES || n != CANI_HEAD + size)
        return "records or size outside the limits";
    for (r = 0, o = CANI_HEAD; r < count; r++) {
        if (o + 16 > n)
            return "a record cut short";
        op = b[o];
        width = rd32(b + o + 8);
        nseg = rd16(b + o + 12);
        npts = rd16(b + o + 14);
        if (op < 1 || op > 3 || b[o + 1] > 2)
            return "a record's operation or caps";
        if (op == 3 && (width < 0x3c000000u || width > 0x41800000u))
            return "a stroke width";
        if (!nseg || nseg > CANI_SEG || npts > CANI_PTS || o + 16 + ((nseg + 3) & ~3u) + 4 * npts > n)
            return "a record's segments or points";
        for (i = 0, pts = 0; i < nseg; i++) {
            s = b[o + 16 + i];
            if (!i && s != 2)
                return "a record not starting with MOVE_TO";
            if (s == 2 || s == 4)
                pts += 1;
            else if (s == 10)
                pts += 2;
            else if (s == 12)
                pts += 3;
            else if (s)
                return "an unknown segment";
        }
        if (pts != npts)
            return "points do not match the segments";
        o += 16 + ((nseg + 3) & ~3u);
        for (i = 0; i < 2 * npts; i++, o += 2) {
            s32 v = (s16)rd16(b + o);

            if (v < 0 || v > 1024)
                return "a point outside the surface";
        }
    }
    return o == n ? 0 : "the records do not fill the section";
}

int look_art(omk_mod *m, u32 index, const void *data, u32 size)
{
    const char *why;
    int rc;

    if (!m || m != owner || index >= OMK_LOOK_MAX || !data)
        return -1;
    why = cani_check(data, size);
    if (why) {
        log_printf(OMK_LOG_WARN, "look: entry %u: cassette picture refused: %s", index + 1, why);
        return -1;
    }
    rc = drv_look_art(index, data, size);
    if (rc)
        log_printf(OMK_LOG_WARN, "look: entry %u: cassette picture not set, code %d", index + 1, rc);
    return rc;
}

void look_drop_module(omk_mod *m)
{
    if (!owner || owner != m)
        return;
    drv_look_set(0);
    drv_look_list(0, 0, 0);
    owner = 0;
    owner_fn = 0;
    optics_known = 0;
    log_printf(OMK_LOG_WARN, "look: %s failed to start, the slot shows the look without entries", m->name);
}

void look_service(u32 now)
{
    drv_look_info s;
    int run;

    if (!owner)
        return;
    if (!prof_owned()) {                    /* the hooks are shared: without a profiles' owner the upkeep is here */
        drv_edp_service();
        if (now - upkeep_ms >= 1000) {
            int rc = drv_prof_service();

            upkeep_ms = now;
            if (rc > 0)
                log_printf(OMK_LOG_INFO, "look: the firmware restored its tables, hooks put back (%d)", rc);
            else if (rc < 0 && !warned) {
                warned = 1;
                log_printf(OMK_LOG_WARN, "look: foreign values in the firmware tables, left alone");
            }
        }
    }
    drv_look_stats(&s);
    {
        drv_prof_info p;

        drv_prof_stats(&p);
        if (p.takes != seen_takes) {        /* a picture: with the look or not, which entry, EXIF texts written so far */
            seen_takes = p.takes;
            log_printf(OMK_LOG_DEBUG, "look: shot %u, look %s, entry %u of %u, EXIF texts %u", p.takes, s.active ? "on" : "off",
                       s.index + 1, s.count, drv_exif_count());
        }
    }
    run = owner_fn && owner->state == MOD_RUNNING;
    if (s.seq != seen.seq) {                /* the user chose the look or left it */
        log_printf(OMK_LOG_INFO, "look: the user turned it %s (entry %u of %u)", s.ui_on ? "on" : "off", s.index, s.count);
        seen.seq = s.seq;
        seen.pick = s.pick;                 /* "on" carries the entry */
        if (run)
            owner_fn(s.ui_on ? OMK_LOOK_ON : OMK_LOOK_OFF, s.ui_on ? s.index : 0, owner_ctx);
    }
    for (;;) {                              /* every dial write since the last loop, with its time */
        u32 t[8], w[8], n, i, lost;

        n = drv_look_trace(&trace_at, &lost, t, w, 8);
        if (lost)
            log_printf(OMK_LOG_DEBUG, "look: dial trace: %u writes lost", lost);
        for (i = 0; i < n; i++)
            log_printf(OMK_LOG_DEBUG, "look: dial %s at %u.%03u ms: value %d, kind %u, step %d",
                       w[i] >> 24 & 1 ? "616 vivid" : "615 hue", t[i] / 1000, t[i] % 1000, (s32)(s16)(w[i] & 0xffff),
                       w[i] >> 28, (s32)(signed char)(w[i] >> 16));
        if (n < 8)
            break;
    }
    if (s.swallowed != seen.swallowed && s.pick == seen.pick && s.dial_last >> 21 & 1)
        log_printf(OMK_LOG_DEBUG, "look: dial 615 hue wrote %u, not a step (%u dial writes)", s.dial_last & 0xffff, s.swallowed);
    seen.swallowed = s.swallowed;
    for (;;) {                              /* the editors' changes, oldest first */
        u32 ev[8], n, i, lost;

        n = drv_look_edit_take(ev, 8, &lost);
        if (lost)
            log_printf(OMK_LOG_WARN, "look: %u editor changes lost", lost);
        for (i = 0; i < n; i++) {
            log_printf(OMK_LOG_DEBUG, "look: edit group %u%s index %u value %d", OMK_EDIT_GROUP_OF(ev[i]),
                       OMK_EDIT_IS_RESET(ev[i]) ? " reset" : "", OMK_EDIT_INDEX_OF(ev[i]), OMK_EDIT_VALUE_OF(ev[i]));
            if (run && s.active)
                owner_fn(OMK_LOOK_EDIT, ev[i], owner_ctx);
        }
        if (n < 8)
            break;
    }
    if (s.pick != seen.pick) {              /* the dials chose an entry (several steps between two loops: the last one) */
        log_printf(OMK_LOG_DEBUG, "look: entry %u of %u chosen (%u steps, %u dial writes, last kind %x, dial %s kind %u value %u%s)",
                   s.index, s.count, s.pick - seen.pick, s.swallowed, s.swallow_kind, s.dial_last >> 16 & 1 ? "616 vivid" : "615 hue",
                   s.dial_last >> 24, s.dial_last & 0xffff,
                   s.dial_last >> 21 & 1 ? ", not a step" : s.dial_last >> 20 & 1 ? ", from the stored hue" : "");
        seen.pick = s.pick;
        if (run && s.active)
            owner_fn(OMK_LOOK_ENTRY, s.index, owner_ctx);
    }
}

/* CREATPRF: creative picture profiles - colour and monochrome profiles of the camera, chosen in the picture mode screen,
 * edited there with the native editor and applied to shooting by core (API 6).
 * The stored picture mode stays a stock one; this module remembers the chosen profile in its store and brings it back at
 * the next power-on. Choice, editing, shooting, live view and own pictures (41, read from the module file on demand: drawn
 * by tools/mkicons_creatprf.py, committed in resources/modules/creatprf/icons). Import over USB: the driver (import.c).
 * Custom modes come later.
 *
 *   [modules]
 *   CREATPRF = on
 *   [CREATPRF]
 *   mono_filters = original     ; or alternative: the softer RED, MAGENTA, BLUE, CYAN at levels 2 and 3 (any case;
 *                               ; another value - a warning and original)
 *
 * The alternative matrices are computed from the camera's own ones (filters.c, read through core: API 18).
 */
#include "omk_api.h"
#include "filters.h"

#define KEY_ACTIVE 1                        /* the active profile, OMK_PROF_* */
#define RES_PICTURE 1                       /* id = OMK_PIC_*: u16 w, u16 h, 4-bit meaning values */
#define ICON_MAX (4 + 64 * 60)              /* the largest picture: 80 x 60 icons, up to 128 x 39 labels */
#define PAIRS 3                             /* kinds with a plain and an edited variant: menu label, panel, shooting screen */

static u8 icon[ICON_MAX];

static const omk_api *api;
static omk_mod *self;
static omk_store *store;
static u32 active;

static const char *kind(u32 prof)
{
    return prof == OMK_PROF_NONE ? "stock mode" : OMK_PROF_IS_MONO(prof) ? "monochrome profile" : "colour profile";
}

static void remember(u32 prof)
{
    active = prof;
    if (store)
        api->store_set(store, KEY_ACTIVE, &active, sizeof(active), OMK_STORE_PERSIST);
}

/* The user picked a profile in the picture mode screen or left it (core task) */
static void on_choice(u32 prof, void *ctx)
{
    (void)ctx;
    remember(prof);
    api->log(self, OMK_LOG_INFO, "%s %u chosen", kind(prof), OMK_PROF_NUM(prof));
}

/* One picture from the module file on the card to core. 1 set, 0 no resource for it, -1 refused */
static int load_picture(u32 pic)
{
    u32 w, h, size, rw, rh;
    int rc;

    if (api->prof_picture_size(pic, &w, &h))
        return 0;
    api->res(self, RES_PICTURE, pic, &size);
    if (!size)
        return 0;
    if (size < 4 || size > ICON_MAX || api->res_read(self, RES_PICTURE, pic, 0, icon, size) != size)
        return -1;
    rw = icon[0] | icon[1] << 8;
    rh = icon[2] | icon[3] << 8;
    if (size != 4 + (rw + 1) / 2 * rh || rh != h || rw > w) {
        api->log(self, OMK_LOG_WARN, "picture %x: %u x %u in the file, the camera takes %u x %u", pic, rw, rh, w, h);
        return -1;
    }
    rc = api->prof_picture(self, pic, rw, rh, icon + 4);
    if (rc)
        api->log(self, OMK_LOG_WARN, "picture %x refused, code %d", pic, rc);
    return rc ? -1 : 1;
}

/* Icons and labels of every profile: what the module file has */
static void load_pictures(u32 colors, u32 monos)
{
    u32 n, v, prof, set = 0, missing = 0, failed = 0;
    int r;

    for (n = 0; n < colors + monos; n++) {
        prof = n < colors ? OMK_PROF_COLOR(n + 1) : OMK_PROF_MONO(n - colors + 1);
        /* the plain pictures only: an "edited" variant not set shows the plain one (core); the camera has no "*" */
        for (v = 0; v < 2 + PAIRS; v++) {
            r = load_picture(v < 2 ? OMK_PIC_MODE(prof, v) : v == 2 ? OMK_PIC_LABEL(prof, 0) : v == 3 ? OMK_PIC_PANEL(prof, 0) :
                             OMK_PIC_SHOOT(prof, 0));
            set += r > 0;
            missing += r == 0;
            failed += r < 0;
        }
    }
    for (v = 0; v < OMK_EDITOR_PICTURES; v++) {    /* the editor's own pictures (the camera has the rest) */
        r = load_picture(OMK_PIC_EDITOR(v));
        set += r > 0;
        missing += r == 0;
        failed += r < 0;
    }
    api->log(self, failed ? OMK_LOG_WARN : OMK_LOG_DEBUG, "%u pictures set, %u not in the module file, %u failed", set, missing,
             failed);
}

static void log_matrix(u32 filter, u32 level, const s16 *m)
{
    api->log(self, OMK_LOG_DEBUG, "monochrome filter %u level %u: %d %d %d / %d %d %d / %d %d %d", filter, level, m[0], m[1],
             m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
}

/* mono_filters = alternative: levels 2 and 3 of RED, MAGENTA, BLUE, CYAN replaced by softer ones computed from the camera's
 * own levels 1 and 2 (filters.c). A filter whose matrices cannot be read keeps the camera's own */
/* a and b the same word, without case */
static int same_word(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20))
            return 0;
    return !*a && !*b;
}

static void mono_filters(void)
{
    static const u8 filters[4] = {3, 6, 7, 8};   /* RED, MAGENTA, BLUE, CYAN */
    const char *opt = api->cfg_str(self, "mono_filters", "original");
    s16 own1[9], own2[9], alt2[9];
    u32 f, set = 0;
    int rc;

    if (!same_word(opt, "alternative")) {
        if (same_word(opt, "original"))
            api->log(self, OMK_LOG_DEBUG, "monochrome filters: the camera's own (mono_filters = %s)", opt);
        else
            api->log(self, OMK_LOG_WARN, "mono_filters = %s: not original or alternative - original", opt);
        return;
    }
    for (f = 0; f < sizeof(filters); f++) {
        if (api->prof_mono_matrix_get(filters[f], 1, own1) || api->prof_mono_matrix_get(filters[f], 2, own2)) {
            api->log(self, OMK_LOG_WARN, "monochrome filter %u: the camera's matrices not readable, its own kept", filters[f]);
            continue;
        }
        alt_level2(own1, own2, alt2);
        rc = api->prof_mono_matrix(self, filters[f], 2, alt2);
        if (!rc) {
            set++;
            log_matrix(filters[f], 2, alt2);
            rc = api->prof_mono_matrix(self, filters[f], 3, own2);   /* level 3 = the camera's level 2 */
            if (!rc) {
                set++;
                log_matrix(filters[f], 3, own2);
            }
        }
        if (rc)
            api->log(self, OMK_LOG_WARN, "monochrome filter %u: refused, code %d", filters[f], rc);
    }
    api->log(self, OMK_LOG_INFO, "monochrome filters: alternative, %u of 8 matrices set", set);
}

int module_init(const omk_api *a, omk_mod *m)
{
    u32 colors = 0, monos = 0;
    int rc;

    if (a->version < OMK_API_VERSION || a->size < sizeof(*a))
        return 1;
    api = a;
    self = m;
    if (!api->prof_count(&colors, &monos)) {
        api->log(self, OMK_LOG_WARN, "this camera has no picture profiles support, nothing to do");
        return 2;
    }
    api->log(self, OMK_LOG_DEBUG, "%u colour and %u monochrome profiles, opening the store", colors, monos);
    store = api->store_open(self, 32);
    if (store)
        api->store_get(store, KEY_ACTIVE, &active, sizeof(active));
    api->log(self, OMK_LOG_DEBUG, "store %s, saved profile %x, enabling the profiles service", store ? "open" : "missing", active);
    rc = api->prof_enable(self, on_choice, 0);
    if (rc) {
        api->log(self, OMK_LOG_ERROR, "picture profiles not available, code %d", rc);
        return 3;
    }
    api->log(self, OMK_LOG_DEBUG, "profiles service enabled, setting the pictures");
    load_pictures(colors, monos);
    mono_filters();
    if (active != OMK_PROF_NONE) {
        rc = api->prof_set(self, active);
        if (rc) {                           /* the picture mode was changed without OMKit: the profile is off */
            api->log(self, OMK_LOG_INFO, "%s %u not restored, code %d", kind(active), OMK_PROF_NUM(active), rc);
            remember(OMK_PROF_NONE);
        } else {
            api->log(self, OMK_LOG_INFO, "%s %u restored", kind(active), OMK_PROF_NUM(active));
        }
    }
    return 0;
}

/* Loading profiles from outside (OM Workspace "Load Color/Monochrome Profile", *.set files, any client of the same backup
 * protocol) for OM-5 Mark II 1.1, at run time, without changes to the firmware file.
 *
 * The camera's own loader (mcon_mode_setting_backup.c, parser 0x61609ecb, PTP and Wi-Fi alike) accepts a line only when the
 * setting is in its white list (table A, u16 IDs; table B, 16-byte entries {type, 0, name offset, value list offset, count},
 * names and value names from component 0x106). The profile lines are not there, and the camera does not announce the
 * function (PTP 0x970B), so Workspace does not offer it. Four jumps, all in code that CPU 0 runs (USB tasks 0x28.., the
 * application task for the parse), installed once - sections 9 and 10 are never loaded again:
 *  - 0x614ba920, the vendor operation registration (it runs at every USB connection, after clearing the list): 0x970B too,
 *    through the driver's table of operations (ptp.c, shared with the debug service);
 *  - 0x6160a694, ID -> table B entry: our entry, only for the value parse (return address 0x6160a850);
 *  - 0x6160a7b7, the miss of name -> ID: our names;
 *  - 0x6160a087, a parsed line about to be applied: the profile lines go through drv_imp_apply.
 * The camera's export keeps its stock table: files from the camera carry only what the stock firmware knows (a design
 * decision). The profile is virtual (111 stays Natural): "select profile n" is the same as the user's choice in the UI;
 * the cells are stock settings, written as the loader writes any line of Current. */
#include "omk_drv.h"
#include "omk_core.h"
#include "omk_log.h"

int drv_ptp_add_raw(u32 code, u32 phase, void *handler);   /* ptp.c */

#define NAME_BASE 0xae563750u               /* component 0x106: setting names, 52 bytes each */
#define VALUE_BASE 0xae589740u              /* value lists {s32 value, char name[52]}: offsets from here (mod 2^32) */
#define TABLE_A 0x6160ac3cu                 /* stock white list: u16 IDs */
#define TABLE_A_N 1573
#define SLOT_CURRENT 0x100
#define ID_COLORTONE 111
#define NATURAL_RAW 2
#define ID_DIAL_COLOR 901                   /* the profile switch positions - this camera has no switch */
#define ID_DIAL_MONO 988
#define ID_SELECT_COLOR 1111                /* the profile number of the switch position */
#define ID_SELECT_MONO 1112
#define CELLS_LO 904                        /* the profile cells 904..1029 */
#define CELLS_HI 1029

/* The 88 IDs of profiles 1-3: 901, 988 and the numbers 1111, 1112; colour 1-3: sharpness, contrast, shading,
 * 12 saturations, tone; mono 1-3: sharpness, contrast, grain, toning, filter, level, shading, tone. List: a stock list of
 * 0x106 (offset, count) or one of ours */
enum { L_OWN901, L_OWN988, L_FILTER, L_SELECT, L_SHARP, L_CONTRAST, L_STEP5, L_TONE, L_STEP03, L_TONING, LISTS };
static const struct {
    u32 off;
    u8 n;
} stock_list[LISTS] = {
    {0, 2}, {0, 2}, {0, 9},
    {0xcb00, 4},                            /* MODE_SELECT_NON.._3 (no _4: three profiles) */
    {0x33d8, 5}, {0x34f0, 5},               /* the lists of 118 MODE_SHARP_VIVID and 117 MODE_CONTRAST_VIVID: -2..2 */
    {0x7f50, 11},                           /* MODE_STEP_M5..P5 */
    {0x7ee0, 15},                           /* the list of 323: MODE_STEP_M7..P7 */
    {0x8068, 4},                            /* MODE_STEP_0..P3 */
    {0x50f0, 5},                            /* the list of 129 MODE_MONOTONECOLOR_MONOCHROME */
};
/* The first value name of each stock list: the component is the one we know */
static const char *const stock_first[LISTS] = {
    0, 0, 0, "MODE_SELECT_NON", "MODE_SHARP_M2", "MODE_CONTRAST_M2", "MODE_STEP_M5", "MODE_STEP_M7", "MODE_STEP_0",
    "MODE_MONOTONECOLOR_NORMAL",
};

typedef struct {
    s32 value;
    char name[52];
} value_rec;

/* Lists 0x106 does not have: the switch positions and all 9 monochrome filters of the profile settings (the camera's
 * pipeline takes 5..8 in a profile). Read by the loader through VALUE_BASE + offset, on CPU 0 */
static const value_rec own901[2] = {{2, "MODE_COLORTONE_NATURAL"}, {11, "MODE_COLORTONE_COLOR_CREATOR_2"}};
static const value_rec own988[2] = {{2, "MODE_COLORTONE_NATURAL"}, {12, "MODE_COLORTONE_MONOCHROME_CREATOR"}};
static const value_rec own_filter[9] = {
    {0, "MODE_MONOTONEFILTER_NORMAL"}, {1, "MODE_MONOTONEFILTER_YELLOW"}, {2, "MODE_MONOTONEFILTER_ORANGE"},
    {3, "MODE_MONOTONEFILTER_RED"}, {6, "MODE_MONOTONEFILTER_MAGENTA"}, {7, "MODE_MONOTONEFILTER_BLUE"},
    {8, "MODE_MONOTONEFILTER_CYAN"}, {4, "MODE_MONOTONEFILTER_GREEN"}, {5, "MODE_MONOTONEFILTER_GREEN_YELLOW"},
};

#define IMP_N 88
u16 drv_imp_ids[IMP_N];                     /* the IDs, then the entries in table B format (thunks.s searches them) */
u32 drv_imp_tab[IMP_N * 4];
static u8 imp_list[IMP_N];
static int imp_ready;

static u32 n_ids;

static void add(u32 id, u32 list)
{
    drv_imp_ids[n_ids] = id;
    imp_list[n_ids] = list;
    n_ids++;
}

static void add_run(u32 first, u32 count, u32 list)
{
    u32 i;

    for (i = 0; i < count; i++)
        add(first + i, list);
}

static int str_is(const volatile char *a, const char *b)
{
    u32 i;

    for (i = 0; i < 52; i++) {
        if (a[i] != b[i])
            return 0;
        if (!b[i])
            return 1;
    }
    return 0;
}

/* The import counters, for the core log (parse task and core task, CPU 0) */
static volatile u32 imp_lines, imp_selects, imp_skipped, imp_names, imp_bad;
static volatile u32 imp_last_prof;

void drv_imp_stats(u32 *lines, u32 *selects, u32 *skipped, u32 *last_prof, u32 *bad)
{
    *lines = imp_lines;
    *selects = imp_selects;
    *skipped = imp_skipped;
    *last_prof = imp_last_prof;
    *bad = imp_bad;
}

static int names_ok(void);

/* Name -> ID, after the stock table missed (thunks.s, the parse task). 0 - not ours either. Component 0x106 is not in memory
 * when core starts (camera, core 0.84: the check at the install failed), so its names and lists are checked here, while the
 * loader itself uses them */
u32 drv_imp_name(const char *name)
{
    u32 i;

    if (drv_cpu())                          /* see drv_imp_apply */
        return 0;
    if (!imp_ready || !name)
        return 0;
    if (!names_ok()) {
        imp_bad++;
        return 0;
    }
    for (i = 0; i < IMP_N; i++)
        if (str_is((const volatile char *)(NAME_BASE + 52 * drv_imp_ids[i]), name)) {
            imp_names++;
            return drv_imp_ids[i];
        }
    return 0;
}

void drv_imp_put(u32 id, u32 value);        /* thunks.s: the loader's write of a Current line (snapshot, working array) */

static int ours(u32 id)
{
    u32 i;

    for (i = 0; i < IMP_N; i++)
        if (drv_imp_ids[i] == id)
            return 1;
    return 0;
}

/* A parsed line, before the camera applies it (thunks.s, the parse task). 1 - handled here (the camera skips it), 0 - the
 * camera applies it as usual */
int drv_imp_apply(u32 id, u32 value, u32 slot)
{
    /* The tables and counters here are core's own variables: cached memory of CPU 0. The parse was only ever seen on
     * CPU 0 (USB tasks, the application task); should it run elsewhere (a Wi-Fi task of another CPU), nothing of core
     * is read or written there and the camera behaves as stock: the names are not known (drv_imp_name), the IDs have
     * no table entry (the trampoline's own check), so no line of ours gets as far as this */
    if (drv_cpu())
        return 0;
    id &= 0xffff;
    if (!imp_ready || !ours(id))
        return 0;
    if (slot != SLOT_CURRENT) {             /* Myset/DialMyset: profiles are not kept in the custom modes */
        imp_skipped++;
        return 1;
    }
    if (id == ID_DIAL_COLOR || id == ID_DIAL_MONO)
        return 1;
    if (id == ID_SELECT_COLOR || id == ID_SELECT_MONO) {
        if (value >= 1 && value <= 3) {     /* as the user's choice: 111 stays Natural, the module is told */
            u32 prof = id == ID_SELECT_COLOR ? OMK_PROF_COLOR(value) : OMK_PROF_MONO(value);

            drv_imp_put(ID_COLORTONE, NATURAL_RAW);
            drv_prof_choose(prof);
            imp_selects++;
            imp_last_prof = prof;
        }
        return 1;
    }
    if (id >= CELLS_LO && id <= CELLS_HI) {
        drv_imp_put(id, value);             /* the core task sees the cell at its next step (drv_prof_prepare) */
        imp_lines++;
        return 1;
    }
    return 0;
}

void drv_h970b(void);                       /* thunks.s */
void drv_tramp_imp_id(void);
void drv_tramp_imp_name(void);
void drv_tramp_imp_apply(void);

#define SITE_ID 0x6160a694
#define SITE_NAME 0x6160a7b7
#define SITE_APPLY 0x6160a087

static const struct {
    u32 addr;
    u8 bytes[8];
} fingerprint[] = {
    {0x614c0156, {0x93, 0xfc, 0xa0, 0x44, 0x21, 0x3a, 0x62, 0x96}},   /* the vendor operation registration */
    {0x602f5243, {0x82, 0x1c, 0xa0, 0x00, 0xc0, 0x16, 0xfa, 0xc8}},   /* Current: the snapshot */
    {0x602f5396, {0x82, 0x1c, 0xa0, 0x00, 0xc0, 0x16, 0xfa, 0xc8}},   /* Current: the working array */
    {0x6160a135, {0x58, 0x0c, 0xf8, 0xbf, 0x54, 0xe3, 0x4e, 0x0c}},   /* the parse loop after a line */
    {0x6160a08d, {0xc8, 0x21, 0xf8, 0xbd, 0x56, 0xcd, 0x9a, 0x05}},   /* after the displaced compare of the slot */
    {0x6160a69a, {0x04, 0x08, 0x2f, 0x25, 0x06, 0x1c, 0xfc, 0xdd}},   /* the stock search after its table address */
    {0x614ba926, {0x85, 0x01, 0x95, 0x00, 0xfc, 0xdc, 0xf0, 0xed}},   /* the registration of 0x970C after its code */
};

/* Component 0x106 (setting names, value lists) is the one we know: some of our names and the first value of each stock
 * list we refer to. Parse task, at every name of ours (a dozen compares) */
static int names_ok(void)
{
    u32 i;

    if (!str_is((const volatile char *)(NAME_BASE + 52 * ID_SELECT_COLOR), "MODE_COLORTONE_DIAL_COLOR_CREATOR_2_SELECT") ||
        !str_is((const volatile char *)(NAME_BASE + 52 * 1029), "MODE_MONOCHROME_CREATOR_TONE_CONTROL_LOW_SET3") ||
        !str_is((const volatile char *)(NAME_BASE + 52 * 904), "MODE_COLOR_CREATOR_2_SHARP_SET1"))
        return 0;
    for (i = 0; i < LISTS; i++)
        if (stock_first[i] && !str_is((const volatile char *)(VALUE_BASE + stock_list[i].off + 4), stock_first[i]))
            return 0;
    return 1;
}

static int install_check(void)
{
    u32 i, k;
    const volatile u16 *a = (const volatile u16 *)TABLE_A;

    for (i = 0; i < sizeof(fingerprint) / sizeof(fingerprint[0]); i++)
        for (k = 0; k < 8; k++)
            if (((const volatile u8 *)fingerprint[i].addr)[k] != fingerprint[i].bytes[k]) {
                log_printf(OMK_LOG_WARN, "import: firmware code at %x differs", fingerprint[i].addr);
                return 1;
            }
    for (i = 0; i < TABLE_A_N; i++)          /* none of ours may be a stock setting of the loader */
        if (ours(a[i])) {
            log_printf(OMK_LOG_WARN, "import: %u is in the camera's own list", a[i]);
            return 4;
        }
    return 0;
}

static u32 list_off(u32 list)
{
    if (list == L_OWN901)
        return (u32)own901 - VALUE_BASE;
    if (list == L_OWN988)
        return (u32)own988 - VALUE_BASE;
    if (list == L_FILTER)
        return (u32)own_filter - VALUE_BASE;
    return stock_list[list].off;
}

/* Core task, with the profiles' install. 0 or a code; nothing is changed unless all is as expected */
int drv_imp_install(void)
{
    static const u8 id_site[5] = {0xfc, 0xdc, 0x88, 0xb8, 0x60};     /* mov 0x6160b888,a0 */
    static const u8 name_site[5] = {0xf1, 0xd8, 0x1c, 0xdf, 0xf0};   /* mov a2,d0; exthu d0; ret [d2,d3,a2,a3],24 */
    static const u8 apply_site[5] = {0x58, 0x48, 0xfa, 0xc8, 0x00};  /* mov (72,sp),d0; cmp 256,d0 */
    u32 i, k;
    int rc;

    if (imp_ready)
        return 0;
    n_ids = 0;
    add(ID_DIAL_COLOR, L_OWN901);
    add(ID_SELECT_COLOR, L_SELECT);
    add(ID_DIAL_MONO, L_OWN988);
    add(ID_SELECT_MONO, L_SELECT);
    add_run(904, 3, L_SHARP);
    add_run(908, 3, L_CONTRAST);
    add_run(912, 3, L_STEP5);
    add_run(928, 36, L_STEP5);
    add_run(976, 9, L_TONE);
    add_run(990, 3, L_SHARP);
    add_run(994, 3, L_CONTRAST);
    add_run(999, 3, L_STEP03);
    add_run(1003, 3, L_TONING);
    add_run(1007, 3, L_FILTER);
    add_run(1012, 3, L_STEP03);
    add_run(1017, 3, L_STEP5);
    add_run(1021, 9, L_TONE);
    if (n_ids != IMP_N)
        return 9;
    rc = install_check();
    if (rc)
        return rc;
    for (i = 0; i < IMP_N; i++) {
        k = imp_list[i];
        drv_imp_tab[4 * i] = 0;             /* type 0: a list of named values */
        drv_imp_tab[4 * i + 1] = 52 * drv_imp_ids[i];
        drv_imp_tab[4 * i + 2] = list_off(k);
        drv_imp_tab[4 * i + 3] = stock_list[k].n;
    }
    imp_ready = 1;                          /* before the jumps: a line may come at once */
    /* the apply first: once names and values of ours are known, no line of ours reaches the camera's own write */
    if ((rc = drv_code_jump(SITE_APPLY, apply_site, (void *)drv_tramp_imp_apply)) != 0)
        return 10 + rc;
    if ((rc = drv_code_jump(SITE_ID, id_site, (void *)drv_tramp_imp_id)) != 0)
        return 20 + rc;
    if ((rc = drv_code_jump(SITE_NAME, name_site, (void *)drv_tramp_imp_name)) != 0)
        return 30 + rc;
    if (drv_ptp_add_raw(0x970b, DRV_PTP_PHASE_NONE, (void *)drv_h970b))
        return 49;
    if ((rc = drv_ptp_install()) != 0)
        return 40 + rc;
    return 0;
}

/* FILMLOOK: film looks in the picture mode screen (API 16). A stock picture mode of the strip -
 * the slot - becomes the film look: its editor column has one item, a rectangle with the preset's name, and the dials step
 * through the presets; live view and the pictures get the preset's ISP colour tables at once (API 13). The presets are the
 * .isp files (OMKit/docs/formats/isp.md) in OMKIT\FILMLOOK\ - 8.3 or long names - shown by the title in their header. The chosen preset and
 * whether the look is on are kept in the module's store and brought back at the next power-on.
 * The editors after the look's item (enable_edits) change the chosen preset's values in a copy of its tables (edits.c),
 * the file is not touched. Shown: the tone curve, the grain item and the optics rows. edits.c also holds the sectors' saturation
 * (Color/Vivid) and the black-and-white filter (Color/Level), but they are not shown (show()), and such edits stored by
 * earlier versions are dropped at load. The edits of every preset are kept in the store by its file name and CRC
 * (save_preset_edits); a changed file drops them.
 *
 *   [modules]
 *   FILMLOOK = on
 *   [FILMLOOK]
 *   slot = ienhance        ; the stock picture mode the look replaces: ienhance, vivid, muted, portrait, monotone, underwater
 *   name = Film Look       ; the look's name in the UI, in place of the slot's (up to 23 characters)
 *   presets =              ; the files in this order (names without .ISP, comma separated); empty - every *.ISP
 *                          ; of the folder by name, at most 64 (more: a warning, the rest left out in the folder's order)
 *   scope = both           ; the ISP tables for: both (live view and pictures), stills, live (any case; another
 *                          ; value - a warning and both)
 *   art = on               ; the presets' cassette pictures; off - the rectangle with the name for every preset
 *   exif = off             ; on: the preset, its CRC and the frame's matrix into the JPEG's UserComment (debugging)
 *   enable_edits = on      ; the editors of the chosen preset in the look's column; off - the presets as in their files
 *   save_preset_edits = on ; keep every preset's edits; off - an edit lasts while its preset stays chosen
 *   grain = on             ; API 17: the grain item (the tile and its strength, per preset) when core has grain tiles
 *   grain_tile =           ; the tile of a preset not edited yet: its container's name (CLASS400 or CLASS400.GRN);
 *                          ; empty - the first by name
 *   optics = on            ; API 20: the optics rows Bloom and Halation (strengths 0..5 per preset) after the grain item
 *
 * The grain (core 1.15, API 17): core keeps the grain tiles (OMKIT\GRAIN\*.GRN) and puts the grain on the look's stills;
 * the grain item after the editors steps the tile (rear dial) and the strength Off, Low, Med, High (front dial), a long
 * OK turns it Off. Kept with the preset's edits (the tile by its file name); a preset without them: Off.
 * Colour grain (core 1.21, [grain] chroma in OMKIT.INI): never on a black-and-white preset - a Cb or Cr gain of 0, or the
 * rows of its matrix (with the edits: a black-and-white filter writes equal rows) within 2 of each other in every column
 * (omk_ispb.h; core applies the same rule to the block it gets). The log line of a preset says which it is.
 * The optics rows (API 20): Bloom and Halation, strengths 0 (off) .. 5 per preset, kept with its edits (EDITS_FORMAT 3;
 * older records: 0); a long OK on a row sets it to 0. Core keeps the chosen preset's strengths for the look's stills; the
 * effects themselves are core's optics layer ([optics] layer = on in OMKIT.INI; without it the rows are not shown).
 */
#include "omk_api.h"
#include "edits.h"
#include "ispfile.h"
#include "omk_ispb.h"

#define KEY_ACTIVE 1                        /* u32: 1 - the look is on */
#define KEY_PRESET 2                        /* the chosen preset's file name with its terminator (13 bytes up to 1.0) */
#define KEY_EDIT 16                         /* KEY_EDIT + k (k < OMK_LOOK_MAX): one preset's edits, edit_rec */
#define RES_PICTURE 1                       /* id = OMK_LOOK_PIC_*: u16 w, u16 h, 4-bit meaning values */
#define PIC_MAX (4 + 64 * 60)
#define MAX_FILE 16384                      /* an OM5M2 .isp: 5.2 KB of tables, up to 6.3 KB of cassette picture */
#define TICK_MS 1                           /* the periodic call at every core loop (250 ms at most, sooner when woken) */
#define FAST_MS 41                          /* the ISP service takes an edit every 40 ms */
#define PART_CURVE (OMK_ISPB_P_GAMMA | OMK_ISPB_P_TONE)   /* the parts the curve editor needs */
#define PART_SECTORS 16
#define NAME 32

typedef struct {
    char file[OMK_NAME_MAX];                /* as on the card: 8.3 in upper case or the long name */
    char title[NAME];
    u32 crc;                                /* CRC-32 of the whole file: identifies it on the computer */
    edits_t e;                              /* the user's edits */
    u32 key;                                /* their store key, 0 - none */
} preset;

typedef struct {
    char file[OMK_NAME_MAX];
    u32 crc;
    edits_t e;
} edit_rec;

#define EDIT_REC_V1 (OMK_NAME_MAX + 4 + EDITS_V1_SIZE)   /* a record of format 1 (FILMLOOK 1.3, 1.4): no grain */
/* A record of format 2 (78 bytes of fields, padded to 80 and stored zeroed) has the size of one of format 3: the optics
 * strengths sit in its padding, the format byte tells them apart */
typedef char edit_rec_size_kept[sizeof(edit_rec) == 80 ? 1 : -1];
#define STORE_SIZE (64 + OMK_LOOK_MAX * (8 + sizeof(edit_rec)))

enum { NOTHING, APPLY, STOCK };

static const omk_api *api;
static omk_mod *self;
static omk_store *store;
static preset *presets;
static const char *names[OMK_LOOK_MAX];
static u32 count, current, active, pending, exif, edits_on, edits_save, edits_warned;
static u32 edit_only, applied_ms;           /* pending is the editors' only (a fast change); when the tables went last */
/* The file being read. A static of the module, not a block from api->alloc: the module's own data is reached through
 * the cache (like its code), a block of the arena is not, and every file is read here and summed twice (its sections,
 * then the whole). Only the core task touches it (module_init, tick, the look's events) */
static u8 buf[MAX_FILE];
/* The chosen preset's ISPB block and the block with the edits: statics for the same reason - every edit copies base to
 * work, changes work and core sums and checks it, 20 KB of reads and writes a dial click that took about 12 ms in blocks
 * of the arena. Word aligned and ISPB_SIZE is a multiple of 4: copied by words (copy_block). Core task only */
static u8 base[ISPB_SIZE] __attribute__((aligned(4)));
static u8 work[ISPB_SIZE] __attribute__((aligned(4)));
static u32 base_of = 0xffffffffu;           /* the preset whose block base holds */
static u32 fresh;                           /* the preset's "on" line is owed (apply): its block is new in base */
static u32 arts_on, arts;                   /* art = on; the cassette pictures core took */
static u32 keys_used[2];                    /* the store keys KEY_EDIT + k taken */
static int same(const char *a, const char *b);
static char (*tiles)[13];                   /* the grain tiles core has (their container files), API 17 */
static u32 ntiles, tile_default;
static u32 optics_on;                       /* API 20: the optics rows shown (optics = on, enable_edits = on) */

/* The grain tile of a preset: the number of its container, -1 - its file is gone */
static s32 tile_of(const preset *p)
{
    u32 i;

    if (!p->e.tile[0])
        return ntiles ? (s32)tile_default : -1;
    for (i = 0; i < ntiles && !same(tiles[i], p->e.tile); i++)
        ;
    return i < ntiles ? (s32)i : -1;
}

/* The chosen preset's grain to core: the grain item shows it, the look's stills get it */
static void grain_publish(void)
{
    int rc;

    if (!ntiles || current >= count)
        return;
    rc = api->look_grain(self, current, edits_on ? tile_of(&presets[current]) : -1, edits_on ? presets[current].e.grain : 0);
    if (rc)
        api->log(self, OMK_LOG_WARN, "grain not given to core, code %d", rc);
}

/* The chosen preset's optics strengths to core: the rows show them, core keeps them for the look's stills. all: every
 * other preset's first - the rows keep each preset's and show them at once when a dial chooses it (the column is drawn
 * in that step, before the preset's apply); core keeps the last ones given as the chosen preset's, so those go last */
static void optics_publish(u32 all)
{
    u32 i;
    int rc;

    if (!optics_on || current >= count)
        return;
    for (i = 0; all && i < count; i++)
        if (i != current)
            api->look_optics(self, i, presets[i].e.optics, OMK_OPTICS_ROWS);
    rc = api->look_optics(self, current, presets[current].e.optics, OMK_OPTICS_ROWS);
    if (rc)
        api->log(self, OMK_LOG_WARN, "optics strengths not given to core, code %d", rc);
}

static u32 get16(const u8 *p)
{
    return p[0] | p[1] << 8;
}

/* The file into buf: bytes read, 0 - cannot open */
static u32 load(const char *file)
{
    omk_file *f = api->file_open(self, file, "r");
    u32 n;

    if (!f)
        return 0;
    n = api->file_read(f, buf, MAX_FILE);
    api->file_close(f);
    return n;
}

static char upper(char c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

/* Names compare without case, as the card does */
static int cmp(const char *a, const char *b)
{
    while (*a && upper(*a) == upper(*b))
        a++, b++;
    return (u8)upper(*a) - (u8)upper(*b);
}

static int same(const char *a, const char *b)
{
    return !cmp(a, b);
}

static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n])
        n++;
    return n;
}

static void copy(char *d, const char *s, u32 max)
{
    u32 i;

    for (i = 0; i + 1 < max && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

/* n bytes from s to d: by words when both are word aligned (base and work always are; a block inside the file buffer only
 * if its section happens to start so), by bytes otherwise - the same bytes either way */
static void copy_block(u8 *d, const u8 *s, u32 n)
{
    u32 i = 0;

    if (!(((uptr)d | (uptr)s) & 3))
        for (; i + 4 <= n; i += 4)
            *(u32 *)(void *)(d + i) = *(const u32 *)(const void *)(s + i);
    for (; i < n; i++)
        d[i] = s[i];
}

/* One preset from its file, read once: checked, its title and CRC kept, its cassette picture (the CANI section; art = on)
 * handed to core while the file is still in buf - the look's service must be taken by now (look_enable). A preset
 * without a picture, or with one core refuses (it logs why), shows the rectangle with its name. 1 added, 0 not */
static int add(const char *file)
{
    const u8 *ispb, *cani;
    const char *why;
    preset *p;
    u32 n, i, ispb_n, cani_n;

    if (count >= OMK_LOOK_MAX) {
        api->log(self, OMK_LOG_WARN, "%s: more than %u presets, skipped", file, OMK_LOOK_MAX);
        return 0;
    }
    for (i = 0; i < count; i++)
        if (same(presets[i].file, file))
            return 0;
    n = load(file);
    if (!n) {
        api->log(self, OMK_LOG_WARN, "%s: cannot open (in OMKIT\\FILMLOOK\\)", file);
        return 0;
    }
    why = n >= MAX_FILE ? "larger than an .isp can be" : isp_check(buf, n, &ispb, &ispb_n, &cani, &cani_n);
    if (why) {
        api->log(self, OMK_LOG_WARN, "%s: %s, skipped", file, why);
        return 0;
    }
    p = &presets[count];
    copy(p->file, file, sizeof(p->file));
    for (i = 0; i < NAME - 1 && buf[ISP_TITLE + i]; i++)
        p->title[i] = buf[ISP_TITLE + i] >= 32 && buf[ISP_TITLE + i] < 127 ? (char)buf[ISP_TITLE + i] : '?';
    p->title[i] = 0;
    if (!i)
        copy(p->title, file, NAME);
    p->crc = isp_crc32(0, buf, n);
    names[count] = p->title;
    if (arts_on && cani && !api->look_art(self, count, cani, cani_n))   /* the entry's number is its place in the list */
        arts++;
    count++;
    api->log(self, OMK_LOG_DEBUG, "preset %u: %s '%s', %u bytes, CRC %08x", count, p->file, p->title, n, p->crc);
    return 1;
}

/* Every .ISP of the module folder, in the order of their names */
static void scan_folder(void)
{
    char (*files)[OMK_NAME_MAX] = api->alloc(self, (OMK_LOOK_MAX + 1) * OMK_NAME_MAX), t[OMK_NAME_MAX];
    u32 n, i, j;

    if (!files)
        return;
    n = api->file_list_long(self, "*.ISP", files, OMK_LOOK_MAX + 1);   /* one more: to know there are too many */
    if (n > OMK_LOOK_MAX) {
        api->log(self, OMK_LOG_WARN, "more than %u .ISP files in OMKIT\\FILMLOOK\\: the first %u in the folder's order are "
                 "listed, the rest left out", OMK_LOOK_MAX, OMK_LOOK_MAX);
        n = OMK_LOOK_MAX;
    }
    for (i = 1; i < n; i++)                 /* insertion sort without case: a few dozen names */
        for (j = i; j > 0 && cmp(files[j - 1], files[j]) > 0; j--) {
            copy(t, files[j], OMK_NAME_MAX);
            copy(files[j], files[j - 1], OMK_NAME_MAX);
            copy(files[j - 1], t, OMK_NAME_MAX);
        }
    for (i = 0; i < n; i++)
        add(files[i]);
    api->free(self, files);
}

/* The next name of `presets = A, B, C` as a file name (A.ISP) into file; *list moves past it. 1, or 0 at the end */
static int next_listed(const char **list, char *file)
{
    const char *l = *list;
    u32 k = 0;

    while (*l && !k) {
        while (*l == ' ' || *l == ',')
            l++;
        for (; *l && *l != ',' && k < OMK_NAME_MAX - 5; l++)
            file[k++] = *l;
        while (*l && *l != ',')
            l++;
        while (k && file[k - 1] == ' ')     /* a name may have spaces inside, not at its ends */
            k--;
    }
    *list = l;
    if (!k)
        return 0;
    file[k++] = '.';
    file[k++] = 'I';
    file[k++] = 'S';
    file[k++] = 'P';
    file[k] = 0;
    return 1;
}

/* presets = A, B, C: those files in that order; otherwise every .ISP of the folder, by name */
static void scan(void)
{
    const char *list = api->cfg_str(self, "presets", "");
    char file[OMK_NAME_MAX];

    if (list[0]) {
        while (next_listed(&list, file))
            add(file);
        return;
    }
    scan_folder();
}

/* 1 - the file is one the list will have: with `presets =` given, one of its names (any file otherwise) */
static int listed(const char *name)
{
    const char *list = api->cfg_str(self, "presets", "");
    char file[OMK_NAME_MAX];

    if (!list[0])
        return 1;
    while (next_listed(&list, file))
        if (same(file, name))
            return 1;
    return 0;
}

/* exif = on: the preset, its edits and the look's matrix (Q7 rows) into the UserComment of the pictures (117 characters at
 * most). The frame's own matrix S+80 is known only when its job runs: core logs it ("isp: look matrix") */
#define TEXT_MAX 160                        /* the text is built here, then cut to the 117 characters UserComment holds */
#define EXIF_MAX 117

static u32 put_str(char *o, u32 k, const char *t)
{
    while (*t && k < TEXT_MAX - 1)
        o[k++] = *t++;
    return k;
}

static u32 put_num(char *o, u32 k, s32 v, u32 hex)
{
    char d[12];
    u32 n = 0, u = v < 0 && !hex ? (u32)-v : (u32)v;

    if (v < 0 && !hex && k < TEXT_MAX - 1)
        o[k++] = '-';
    do {
        d[n++] = "0123456789abcdef"[u % (hex ? 16 : 10)];
        u /= hex ? 16 : 10;
    } while (u || (hex && n < 8));
    while (n && k < TEXT_MAX - 1)
        o[k++] = d[--n];
    return k;
}

/* "FILMLOOK <file> CRC <crc> [E [t=Sh,Mid,Hi] [v=12 shifts] [l=filter,level] [g=tile,strength] [o=bloom,halation]]
 * [F <matrix by rows>] '<title>'": the edits
 * right after the CRC, so a long name cuts the matrix or the title rather than them; a text cut to EXIF_MAX ends in '~' */
static void exif_text(const u8 *ispb)
{
    char t[TEXT_MAX];
    u32 k = 0, i;
    const preset *p = &presets[current];

    if (!exif)
        return;
    k = put_str(t, k, "FILMLOOK ");
    for (i = 0; p->file[i] && k < TEXT_MAX - 1; i++)     /* the fields are split at spaces: none in the name */
        t[k++] = p->file[i] == ' ' ? '_' : p->file[i];
    k = put_str(t, k, " CRC ");
    k = put_num(t, k, (s32)p->crc, 1);
    if (edits_on && edits_changed(&p->e)) {  /* the edits: the model gets them too */
        k = put_str(t, k, " E");
        if (p->e.tone[0] || p->e.tone[1] || p->e.tone[2])
            for (i = 0; i < 3; i++) {
                k = put_str(t, k, i ? "," : " t=");
                k = put_num(t, k, p->e.tone[i], 0);
            }
        if (p->e.vivid_set)
            for (i = 0; i < 12; i++) {      /* all 12 shifts in order */
                k = put_str(t, k, i ? "," : " v=");
                k = put_num(t, k, p->e.vivid[i], 0);
            }
        if (p->e.filter) {
            k = put_str(t, k, " l=");
            k = put_num(t, k, p->e.filter, 0);
            k = put_str(t, k, ",");
            k = put_num(t, k, p->e.level, 0);
        }
        if (p->e.grain && tile_of(p) >= 0) {  /* the grain: its container (without .GRN) and strength */
            k = put_str(t, k, " g=");
            for (i = 0; tiles[tile_of(p)][i] && tiles[tile_of(p)][i] != '.' && k < TEXT_MAX - 1; i++)
                t[k++] = tiles[tile_of(p)][i];
            k = put_str(t, k, ",");
            k = put_num(t, k, p->e.grain, 0);
        }
        if (p->e.optics[OMK_OPTICS_BLOOM] || p->e.optics[OMK_OPTICS_HALATION]) {   /* the optics rows' strengths */
            k = put_str(t, k, " o=");
            k = put_num(t, k, p->e.optics[OMK_OPTICS_BLOOM], 0);
            k = put_str(t, k, ",");
            k = put_num(t, k, p->e.optics[OMK_OPTICS_HALATION], 0);
        }
    }
    if (ispb && (get16(ispb + 2) & 1)) {    /* the matrix present */
        k = put_str(t, k, " F");
        for (i = 0; i < 9; i++) {
            k = put_str(t, k, i % 3 ? "," : " ");
            k = put_num(t, k, (s16)get16(ispb + 8 + 2 * i), 0);
        }
    }
    k = put_str(t, k, " '");
    k = put_str(t, k, p->title);
    k = put_str(t, k, "'");
    if (k > EXIF_MAX) {
        k = EXIF_MAX;
        t[k - 1] = '~';
    }
    t[k] = 0;
    api->exif_comment(self, t);
}

/* What the editors show for the chosen preset (its block in base, the edited one in work) */
static void show(void)
{
    omk_look_edits st;
    u32 parts = get16(base + OMK_ISPB_PRESENT), groups = 0;
    int rc;

    /* an editor only where it can act: the curve needs the gamma and the tone. Only the curve is shown: Color/Vivid and
     * Color/Level are taken out until one scheme for the sector editors (with hue and luminosity) and the toned
     * black-and-white presets is decided; edits.c keeps their arithmetic. When they come back: Color/Level only where
     * edits_mono(base), Color/Vivid only where not - the one rule (omk_ispb.h) */
    if (edits_on && (parts & PART_CURVE) == PART_CURVE)
        groups |= 1u << OMK_EDIT_TONE;
    if (edits_on && ntiles)
        groups |= 1u << OMK_EDIT_GRAIN;     /* the grain item: whatever the preset */
    if (optics_on)
        groups |= 1u << OMK_EDIT_OPTICS;    /* the optics rows: whatever the preset */

    edits_show(base, work, &presets[current].e, groups, &st);
    st.entry = current;
    rc = api->look_edits(self, &st);
    if (rc && groups && !edits_warned) {
        edits_warned = 1;
        api->log(self, OMK_LOG_WARN, "the editors are not shown, code %d", rc);
    }
}

/* The chosen preset's tables, with its edits, to the ISP service. Its block is read from the card once, when it becomes
 * the chosen one (at power-on restore_early has read it already); an edit only changes the copy. The preset's "on" line
 * (fresh) is kept over a refusal as too soon (-2), when the tables go at a later tick. 0, or the service's code (-2: too
 * soon, again at the next tick) */
static int apply(void)
{
    const u8 *ispb;
    const char *why;
    u32 n, ispb_n;
    int rc;

    if (current >= count)
        return -1;
    if (base_of != current) {
        n = load(presets[current].file);
        why = !n ? "cannot open" : n >= MAX_FILE ? "too large" : isp_check(buf, n, &ispb, &ispb_n, 0, 0);
        if (!why && ispb_n != ISPB_SIZE)
            why = "the ISPB section is not of the OM5M2 layout";
        if (why) {
            api->log(self, OMK_LOG_ERROR, "%s: %s, the camera's own tables stay", presets[current].file, why);
            return -1;
        }
        copy_block(base, ispb, ISPB_SIZE);
        base_of = current;
        fresh = 1;
    }
    copy_block(work, base, ISPB_SIZE);
    if (edits_on)
        edits_apply(work, &presets[current].e);
    rc = api->isp_override(self, work, ISPB_SIZE);
    if (rc == -2)
        return rc;
    if (rc)
        api->log(self, OMK_LOG_ERROR, "%s: tables refused, code %d", presets[current].file, rc);
    else if (fresh)
        api->log(self, OMK_LOG_INFO, "preset %u '%s' (%s, CRC %08x) on%s, %s", current + 1, presets[current].title,
                 presets[current].file, presets[current].crc, edits_on && edits_changed(&presets[current].e) ? ", edited" : "",
                 edits_mono(work) ? "black and white (no colour grain)" : "colour");
    fresh = 0;                              /* told, or refused for good (logged above) */
    if (!rc) {
        exif_text(work);
        show();
        grain_publish();
        optics_publish(0);
    }
    return rc;
}

/* A preset's edits into the store, or out of it when there are none left. Every change: the store writes the file a few
 * seconds after the last one */
static void save_edits(u32 i)
{
    preset *p = &presets[i];
    edit_rec r;
    u32 k, j;

    if (!store || !edits_save)
        return;
    if (!edits_changed(&p->e)) {
        if (p->key) {
            api->store_del(store, (u16)p->key);
            keys_used[(p->key - KEY_EDIT) / 32] &= ~(1u << (p->key - KEY_EDIT) % 32);
            p->key = 0;
        }
        return;
    }
    if (!p->key) {
        for (k = 0; k < OMK_LOOK_MAX && keys_used[k / 32] >> k % 32 & 1; k++)
            ;
        if (k == OMK_LOOK_MAX) {            /* every key taken: one of them by a file that is not on the card now
                                               (load_edits keeps such records) - the first of those gives way */
            for (k = 0; k < OMK_LOOK_MAX; k++) {
                for (j = 0; j < count && presets[j].key != KEY_EDIT + k; j++)
                    ;
                if (j == count)
                    break;
            }
            if (k == OMK_LOOK_MAX) {
                api->log(self, OMK_LOG_WARN, "%s: no room for its edits in the store", p->file);
                return;
            }
            api->store_del(store, (u16)(KEY_EDIT + k));
            api->log(self, OMK_LOG_INFO, "%s: its edits take the place of a preset's that is not on the card", p->file);
        }
        keys_used[k / 32] |= 1u << k % 32;
        p->key = KEY_EDIT + k;
    }
    for (j = 0; j < sizeof(r); j++)         /* the record goes to the card whole: no stack bytes after the name's end
                                               or in the padding */
        ((u8 *)&r)[j] = 0;
    copy(r.file, p->file, sizeof(r.file));
    r.crc = p->crc;
    r.e = p->e;
    r.e.format = EDITS_FORMAT;
    if (api->store_set(store, (u16)p->key, &r, sizeof(r), OMK_STORE_PERSIST))
        api->log(self, OMK_LOG_WARN, "%s: edits not stored", p->file);
}

/* The record of store key KEY_EDIT + k into r: 1, or 0 - none there (or not a record of this layout or of format 1) */
static int rec_get(u32 k, edit_rec *r)
{
    u32 i;
    int n;

    for (i = 0; i < sizeof(*r); i++)
        ((u8 *)r)[i] = 0;
    n = api->store_get(store, (u16)(KEY_EDIT + k), r, sizeof(*r));
    if (n != (int)sizeof(*r) && !(n == EDIT_REC_V1 && r->e.format == 1))
        return 0;                           /* format 1 (no grain): its fields as they are, the grain 0 - Off */
    if (r->e.format < 3)                    /* formats 1, 2: no optics strengths (2: the padding there) - 0, off */
        for (i = 0; i < OMK_OPTICS_ROWS; i++)
            r->e.optics[i] = 0;
    r->file[sizeof(r->file) - 1] = 0;
    return 1;
}

/* A record's edits as they are taken: what the editors can set - a damaged record must not go further */
static void rec_clean(edits_t *e)
{
    u32 n;

    e->tile[GRAIN_FILE - 1] = 0;
    if (e->grain > 3)
        e->grain = 0;
    for (n = 0; n < 3; n++)
        e->tone[n] = e->tone[n] < -7 ? -7 : e->tone[n] > 7 ? 7 : e->tone[n];
    for (n = 0; n < OMK_OPTICS_ROWS; n++)
        if (e->optics[n] > OMK_OPTICS_MAX)
            e->optics[n] = 0;
}

/* The stored edits back: by the file name, if the file is the same (CRC); a changed file drops them. Records of files not
 * on the card now stay (the preset may come back), until save_edits needs a key and none is free */
static void load_edits(void)
{
    edit_rec r;
    u32 k, i, got = 0;

    for (k = 0; k < OMK_LOOK_MAX; k++) {
        if (!rec_get(k, &r))
            continue;
        keys_used[k / 32] |= 1u << k % 32;
        for (i = 0; i < count && !same(presets[i].file, r.file); i++)
            ;
        if (i == count)
            continue;
        if (r.e.format < 1 || r.e.format > EDITS_FORMAT) {    /* FILMLOOK 1.2 kept Color/Vivid as values in place of the
                                                                   preset's (format 0) */
            api->log(self, OMK_LOG_INFO, "%s: edits of an older FILMLOOK dropped", presets[i].file);
            api->store_del(store, (u16)(KEY_EDIT + k));
            keys_used[k / 32] &= ~(1u << k % 32);
            continue;
        }
        if (r.crc != presets[i].crc) {
            api->log(self, OMK_LOG_INFO, "%s: the file changed, its edits dropped", presets[i].file);
            api->store_del(store, (u16)(KEY_EDIT + k));
            keys_used[k / 32] &= ~(1u << k % 32);
            continue;
        }
        rec_clean(&r.e);
        presets[i].e = r.e;
        presets[i].key = KEY_EDIT + k;
        if (edits_changed(&r.e) & (1u << OMK_EDIT_VIVID | 1u << OMK_EDIT_LEVEL)) {   /* editors taken out (see show) */
            edits_reset(&presets[i].e, OMK_EDIT_VIVID);
            edits_reset(&presets[i].e, OMK_EDIT_LEVEL);
            api->log(self, OMK_LOG_INFO, "%s: Color/Vivid and Color/Level edits dropped", presets[i].file);
            save_edits(i);
            if (!presets[i].key)
                continue;
        }
        got++;
    }
    if (got)
        api->log(self, OMK_LOG_INFO, "edits of %u presets restored%s", got, edits_on ? "" : " (enable_edits = off: not used)");
}

/* Power-on, the look was on: the stored preset's tables to the ISP service before the other presets are read (a few
 * dozen files, about a second) - a picture taken meanwhile has the look's colours. Only what does not need the list: the
 * look on (core checks the stored picture mode) and the tables with the preset's stored edits; the entry, the editors'
 * values, the grain and the EXIF text follow at the first tick, as ever (apply). The file is read once more by scan()
 * with all the others: one open, and add() stays the only place a preset comes from. Anything wrong here (the file gone
 * or damaged, not in `presets =`) is left to the usual way after the list: this function reports nothing then.
 * look_tried / look_rc: look_set was called here, and its answer - module_init does not ask twice */
static u32 look_tried, early, early_crc;    /* early: the tables of the stored preset (file CRC early_crc) are in force */
static int look_rc;

static void restore_early(const char *saved)
{
    const u8 *ispb;
    edit_rec r;
    edits_t e;
    u32 n, k, ispb_n;

    if (!saved[0] || !listed(saved))
        return;
    n = load(saved);
    if (!n || n >= MAX_FILE || isp_check(buf, n, &ispb, &ispb_n, 0, 0) || ispb_n != ISPB_SIZE)
        return;
    look_tried = 1;
    look_rc = api->look_set(self, 1);
    if (look_rc)                            /* the picture mode was changed without OMKit: said after the list */
        return;
    early_crc = isp_crc32(0, buf, n);
    copy_block(base, ispb, ISPB_SIZE);
    copy_block(work, base, ISPB_SIZE);
    for (n = 0; n < sizeof(e); n++)
        ((u8 *)&e)[n] = 0;
    /* the preset's edits as load_edits will take them: the same file (name and CRC), a format still read, without the
     * editors taken out. Should the two ever differ, apply() puts it right at the first tick */
    for (k = 0; store && edits_save && edits_on && k < OMK_LOOK_MAX; k++)
        if (rec_get(k, &r) && same(r.file, saved) && r.crc == early_crc && r.e.format >= 1 && r.e.format <= EDITS_FORMAT) {
            rec_clean(&r.e);
            e = r.e;
            edits_reset(&e, OMK_EDIT_VIVID);
            edits_reset(&e, OMK_EDIT_LEVEL);
        }
    if (edits_on)
        edits_apply(work, &e);
    if (api->isp_override(self, work, ISPB_SIZE))   /* core logs a refusal; apply() tries again and says more */
        return;
    early = 1;
    api->log(self, OMK_LOG_INFO, "%s (CRC %08x): its tables are on, before the presets are read", saved, early_crc);
}

static void remember(void)
{
    if (!store)
        return;
    api->store_set(store, KEY_ACTIVE, &active, sizeof(active), OMK_STORE_PERSIST);
    if (current < count)
        api->store_set(store, KEY_PRESET, presets[current].file, slen(presets[current].file) + 1, OMK_STORE_PERSIST);
}

/* Core task, about every TICK_MS: what the events asked for */
static void tick(void *ctx)
{
    u32 since;
    int rc;

    (void)ctx;
    if (pending == NOTHING)
        return;
    if (pending == STOCK && exif)
        api->exif_comment(self, 0);
    /* An edit sooner than FAST_MS after the tables went last: the ISP service would refuse it as too soon (-2), so the
     * wait comes first and the tables are built and given once - not built, refused, and built again after the wait.
     * The edit goes 40 ms after the one before, not at the next loop */
    if (pending == APPLY && edit_only) {
        since = api->ms() - applied_ms;
        if (since < FAST_MS)
            api->sleep(FAST_MS - since);
    }
    rc = pending == APPLY ? apply() : api->isp_override(self, 0, 0);
    if (rc != -2) {
        pending = NOTHING;
        edit_only = 0;
        applied_ms = api->ms();
    }
}

/* The user chose the look, left it, or a dial chose another preset (core task) */
static void on_look(u32 event, u32 arg, void *ctx)
{
    (void)ctx;
    switch (event) {
    case OMK_LOOK_ON:
        edit_only = 0;
        active = 1;
        current = arg < count ? arg : 0;
        pending = count ? APPLY : NOTHING;
        api->log(self, OMK_LOG_INFO, "look on, preset %u of %u", current + 1, count);
        break;
    case OMK_LOOK_OFF:
        active = 0;
        pending = STOCK;
        api->log(self, OMK_LOG_INFO, "look off");
        break;
    case OMK_LOOK_ENTRY:
        if (arg < count) {
            if (!edits_save && current < count && arg != current) {
                edits_reset(&presets[current].e, OMK_EDIT_GROUPS);    /* an edit lasts while its preset stays chosen */
                optics_publish(0);          /* the rows keep its strengths (now 0) for a dial's way back to it */
            }
            current = arg;
            pending = APPLY;
            edit_only = 0;
        }
        break;
    case OMK_LOOK_EDIT:                     /* one or many in a row: applied once, at the tick after them. Only the curve
                                               is shown (see show) */
        if (edits_on && current < count && OMK_EDIT_GROUP_OF(arg) == OMK_EDIT_GRAIN && ntiles) {
            preset *p = &presets[current];
            s32 v = OMK_EDIT_VALUE_OF(arg);

            if (OMK_EDIT_INDEX_OF(arg) == 0 && v >= 0 && (u32)v < ntiles)
                copy(p->e.tile, tiles[v], GRAIN_FILE);
            else if (OMK_EDIT_INDEX_OF(arg) == 1 && v >= 0 && v <= 3)
                p->e.grain = (u8)v;
            else
                return;
            api->log(self, OMK_LOG_INFO, "preset %u: grain %s, strength %u", current + 1, tile_of(p) >= 0 ?
                     tiles[tile_of(p)] : "(none)", p->e.grain);
            save_edits(current);
            grain_publish();
            if (base_of == current)
                exif_text(work);
            return;
        }
        if (optics_on && current < count && OMK_EDIT_GROUP_OF(arg) == OMK_EDIT_OPTICS) {
            preset *p = &presets[current];
            s32 v = OMK_EDIT_VALUE_OF(arg);
            u32 i = OMK_EDIT_INDEX_OF(arg);

            if (i >= OMK_OPTICS_ROWS || v < 0 || v > OMK_OPTICS_MAX || p->e.optics[i] == (u8)v)
                return;
            p->e.optics[i] = (u8)v;
            api->log(self, OMK_LOG_INFO, "preset %u: %s %d", current + 1, i == OMK_OPTICS_BLOOM ? "bloom" : "halation", v);
            save_edits(current);
            optics_publish(0);
            if (base_of == current)
                exif_text(work);
            return;
        }
        if (!edits_on || current >= count || OMK_EDIT_GROUP_OF(arg) != OMK_EDIT_TONE ||
            !edits_event(&presets[current].e, arg))
            return;
        save_edits(current);
        if (pending == NOTHING)
            edit_only = 1;
        pending = APPLY;
        return;
    default:
        return;
    }
    remember();
    tick(0);                                /* at once if the ISP service takes it now, otherwise at the next tick */
}

/* The grain tiles core has (API 17): the grain item and the look's grain; the default tile of a preset not edited */
static void grain_init(void)
{
    const char *def;
    char want[GRAIN_FILE];
    u32 i, k;

    if (api->version < 17 || !edits_on || !api->cfg_bool(self, "grain", 1)) {
        api->log(self, OMK_LOG_INFO, "grain: %s", api->version < 17 ? "core before API 17" : "off (grain or enable_edits)");
        return;
    }
    tiles = api->alloc(self, OMK_GRAIN_TILES * 13);
    if (!tiles)
        return;
    ntiles = api->grain_list(self, tiles, OMK_GRAIN_TILES);
    if (!ntiles) {
        api->log(self, OMK_LOG_INFO, "grain: no tiles from core");
        return;
    }
    def = api->cfg_str(self, "grain_tile", "");
    for (k = 0; def[k] && def[k] != '.' && k < 8; k++)
        want[k] = def[k];
    want[k] = 0;
    for (i = 0; k && i < ntiles; i++) {     /* by the base name, with or without .GRN */
        for (k = 0; want[k] && upper(want[k]) == upper(tiles[i][k]); k++)
            ;
        if (!want[k] && tiles[i][k] == '.')
            break;
        k = 1;
    }
    tile_default = def[0] && i < ntiles ? i : 0;
    if (def[0] && i == ntiles)
        api->log(self, OMK_LOG_WARN, "grain_tile = %s: no such tile, the first one is the default", def);
    api->log(self, OMK_LOG_INFO, "grain: %u tiles, the default %s", ntiles, tiles[tile_default]);
}

/* One picture from the module file to core. 1 set, 0 none in the file, -1 refused */
static int load_picture(u32 pic, u8 *px)
{
    u32 w, h, size, rw, rh;
    int rc;

    if (api->look_picture_size(pic, &w, &h))
        return 0;
    api->res(self, RES_PICTURE, pic, &size);
    if (!size)
        return 0;
    if (size < 4 || size > PIC_MAX || api->res_read(self, RES_PICTURE, pic, 0, px, size) != size)
        return -1;
    rw = px[0] | px[1] << 8;
    rh = px[2] | px[3] << 8;
    if (size != 4 + (rw + 1) / 2 * rh || rh != h || rw > w) {
        api->log(self, OMK_LOG_WARN, "picture %u: %u x %u in the file, the camera takes %u x %u", pic, rw, rh, w, h);
        return -1;
    }
    rc = api->look_picture(self, pic, rw, rh, px + 4);
    return rc ? -1 : 1;
}

static void load_pictures(void)
{
    u8 *px = api->alloc(self, PIC_MAX);
    u32 pic, set = 0, missing = 0, failed = 0;
    int r;

    if (!px)
        return;
    for (pic = 0; pic < OMK_LOOK_PICTURES; pic++) {
        r = load_picture(pic, px);
        set += r > 0;
        missing += r == 0;
        failed += r < 0;
    }
    api->free(self, px);
    api->log(self, failed ? OMK_LOG_WARN : OMK_LOG_DEBUG, "%u pictures set, %u not in the module file, %u failed", set, missing,
             failed);
}

/* The order: the store and the two services first (their hooks go in at isp_scope and look_enable), so that a look that
 * was on at power-off gets its tables at once (restore_early) and the presets' cassette pictures can go to core while
 * each file is read (add); then the presets, their stored edits, the grain tiles, the pictures and the list */
int module_init(const omk_api *a, omk_mod *m)
{
    char saved[OMK_NAME_MAX];
    const char *slot, *sc;
    u32 scope, i;
    int rc;

    if (a->version < 16 || a->size < sizeof(*a))
        return 1;
    api = a;
    self = m;
    presets = api->alloc(self, OMK_LOOK_MAX * sizeof(preset));
    if (!presets)
        return 2;
    for (i = 0; i < OMK_LOOK_MAX * sizeof(preset); i++)
        ((u8 *)presets)[i] = 0;
    edits_on = api->cfg_bool(self, "enable_edits", 1);
    edits_save = api->cfg_bool(self, "save_preset_edits", 1);
    arts_on = api->cfg_bool(self, "art", 1);   /* art = off: the rectangle with the name for every preset */
    store = api->store_open(self, STORE_SIZE);
    saved[0] = 0;
    if (store) {
        api->store_get(store, KEY_ACTIVE, &active, sizeof(active));
        if (api->store_get(store, KEY_PRESET, saved, sizeof(saved)) > 0)
            saved[sizeof(saved) - 1] = 0;
        else
            saved[0] = 0;
    }
    sc = api->cfg_str(self, "scope", "both");
    scope = same(sc, "stills") ? OMK_ISP_STILLS : same(sc, "live") ? OMK_ISP_LIVE : OMK_ISP_LIVE | OMK_ISP_STILLS;
    if (!same(sc, "stills") && !same(sc, "live") && !same(sc, "both"))
        api->log(self, OMK_LOG_WARN, "scope = %s: not both, stills or live - both", sc);
    rc = api->isp_scope(self, scope);       /* the first call of the ISP service: its hooks go in now */
    if (rc) {
        api->log(self, OMK_LOG_ERROR, "the ISP service is not available, code %d (another module may hold it)", rc);
        return 3;
    }
    slot = api->cfg_str(self, "slot", "ienhance");
    rc = api->look_enable(self, slot, on_look, 0);
    if (rc) {
        api->log(self, OMK_LOG_ERROR, "the film look is not available in slot %s, code %d", slot, rc);
        return 4;
    }
    if (active)
        restore_early(saved);
    scan();
    api->log(self, count ? OMK_LOG_INFO : OMK_LOG_WARN, "%u presets in OMKIT\\FILMLOOK\\", count);
    if (arts_on)
        api->log(self, OMK_LOG_INFO, "%u cassette pictures, %u presets without one", arts, count - arts);
    else
        api->log(self, OMK_LOG_INFO, "art = off: no cassette pictures");
    for (i = 0; i < count; i++)
        if (same(presets[i].file, saved))
            current = i;
    if (early && current < count && same(presets[current].file, saved) && presets[current].crc == early_crc) {
        base_of = current;                  /* base holds this preset's block already: apply() does not read the file */
        fresh = 1;
    }
    if (store && edits_save)
        load_edits();
    grain_init();
    optics_on = api->version >= 20 && edits_on && api->cfg_bool(self, "optics", 1);
    api->log(self, OMK_LOG_INFO, "optics rows: %s", optics_on ? "on" : api->version < 20 ? "core before API 20" :
             "off (optics or enable_edits)");
    load_pictures();
    rc = api->look_name(self, api->cfg_str(self, "name", "Film Look"));
    if (rc)
        api->log(self, OMK_LOG_WARN, "the look's name not set, code %d", rc);
    exif = api->cfg_bool(self, "exif", 0);
    rc = api->look_list(self, names, count, current);
    if (rc)
        api->log(self, OMK_LOG_WARN, "presets not given to the editor, code %d", rc);
    optics_publish(1);                      /* now, not at the first apply: the column the UI builds when the user steps
                                               onto the look has the optics rows only once core has had their values */
    if (active && count) {
        rc = look_tried ? look_rc : api->look_set(self, 1);
        if (rc) {                           /* the picture mode was changed without OMKit: the look is off */
            api->log(self, OMK_LOG_INFO, "look not restored, code %d", rc);
            active = 0;
            remember();
        } else if (look_tried && !api->look_get()) {
            /* on since restore_early, and off again by now: the user left the look in the picture mode screen while the
             * presets were read. No event comes for it (the list above took the look's state as seen) */
            api->log(self, OMK_LOG_INFO, "look turned off while the presets were read");
            active = 0;
            pending = early ? STOCK : NOTHING;
            remember();
        } else {
            pending = APPLY;
            api->log(self, OMK_LOG_INFO, "look restored, preset %u '%s'", current + 1, presets[current].title);
        }
    } else if (active) {
        if (look_tried && !look_rc)         /* on since restore_early, and no preset made it into the list */
            api->look_set(self, 0);
        pending = early ? STOCK : NOTHING;
        active = 0;
        remember();
    }
    api->every(self, TICK_MS, tick, 0);
    return 0;
}

/* OMKIT.INI is parsed in place: lines are cut with zeros, the table points into the text. One pass at startup. */
#include "omk_core.h"
#include "omk_log.h"

#define CFG_NAME "OMKIT.INI"
#define CFG_MAX 8192
#define CFG_ENTRIES 128
#define CFG_ONCE_MAX 1024                   /* a one-shot file of one section's keys (cfg_once) */

/* The sample: one text for both build kinds, a plain sequence of string literals (tools/cardini.py reads it out of this
 * file for the card bundle's OMKIT.INI). It names no setting of the debug build */
static const char cfg_template[] =
    "; OMKit settings. Lines starting with ; or # are comments.\r\n"
    "; The file is read when the camera is switched on. Delete it, and core will create this sample again.\r\n"
    "\r\n"
    "[core]\r\n"
    "; on | off - whether to write the log OMKIT\\LOG\\CORE.LOG on the card\r\n"
    "log = on\r\n"
    "; error | warn | info - how much detail (debug - in a debug build of core only)\r\n"
    "log_level = info\r\n"
    "; on - if the arena runs out of space, take small module buffers (up to 16K) from the firmware heap.\r\n"
    "; Requires fw_heap_limit. The heap is shared with shooting: enable only if the arena is too small for modules\r\n"
    "fw_heap = off\r\n"
    "fw_heap_limit = 256K\r\n"
    "\r\n"
    "[grain]\r\n"
    "; the grain of the film look's stills (a look module such as FILMLOOK picks the tile and the strength).\r\n"
    "; film - weaker in the shadows and highlights, as on film; flat - the same in every tone;\r\n"
    "; off - the same in every tone by the older route (the fallback if film or flat fail)\r\n"
    "mask = film\r\n"
    "; colour grain on colour presets, 0..1024 (0 - none; never on black and white presets)\r\n"
    "chroma = 0\r\n"
    "\r\n"
    "[optics]\r\n"
    "; Bloom and Halation on the film look's stills. The strength of each is the preset's Bloom and Halation row\r\n"
    "; in the look's column: 0 - off, 1..5. off here - no Bloom and Halation at all\r\n"
    "layer = on\r\n"
    "; The values below are the defaults. The glow starts at the code *_threshold of the picture (0..254);\r\n"
    "; *_sigma - its size in pixels of the picture; *_gain - the light added at strength 5, percent\r\n"
    "; Bloom: a diffusion filter. bloom_tail percent of its light in a long tail of bloom_tail_sigma pixels;\r\n"
    "; bloom_dark - percent less on bright places; bloom_max - at most this percent of white;\r\n"
    "; bloom_colour - percent of the colour of its sources (0 - neutral)\r\n"
    "bloom_threshold = 180\r\n"
    "bloom_sigma = 90\r\n"
    "bloom_gain = 110\r\n"
    "bloom_dark = 80\r\n"
    "bloom_tail = 50\r\n"
    "bloom_tail_sigma = 400\r\n"
    "bloom_haze = 0\r\n"
    "bloom_toe = 0\r\n"
    "bloom_max = 38\r\n"
    "bloom_colour = 300\r\n"
    "; Halation: the red glow around lamps. halation_cb, halation_cr - its colour where it is strong,\r\n"
    "; halation_edge_cb, halation_edge_cr - at its weak edge, the strong colour from the code halation_edge_code;\r\n"
    "; halation_core = 1 - a lamp's core stays white; halation_toe, halation_max - percent of white\r\n"
    "halation_threshold = 240\r\n"
    "halation_sigma = 45\r\n"
    "halation_gain = 220\r\n"
    "halation_dark = 100\r\n"
    "halation_cb = -24\r\n"
    "halation_cr = 42\r\n"
    "halation_edge_cb = -40\r\n"
    "halation_edge_cr = 75\r\n"
    "halation_edge_code = 70\r\n"
    "halation_core = 1\r\n"
    "halation_toe = 3\r\n"
    "halation_max = 60\r\n"
    "; The source: a blown pixel stands for this many whites (0 - only the excess over the threshold);\r\n"
    "; emission - for both when given, else each its own\r\n"
    "emission = 16\r\n"
    "bloom_emission = 4\r\n"
    "halation_emission = 32\r\n"
    "; 1 - the halation takes its colour before Bloom is added\r\n"
    "halation_first = 1\r\n"
    "; The strengths 1..5 in percent of the gains\r\n"
    "bloom_steps = 20, 32, 50, 71, 100\r\n"
    "halation_steps = 10, 19, 34, 80, 140\r\n"
    "; A day scene (the picture's mean code from..to, or the percent of it at code 200 from..to) may take\r\n"
    "; day_bloom and day_halation percent of the gains; 100 - the same by day and by night\r\n"
    "day_mean = 95, 125\r\n"
    "day_share = 6, 14\r\n"
    "day_bloom = 100\r\n"
    "day_halation = 100\r\n"
    "; auto - the codes on the scale of the look's white; off - as they are; 160..255 - a fixed white\r\n"
    "white = auto\r\n"
    "; first - of a RAW+JPEG press only the JPEG gets the layer; both - the preview in the ORF too\r\n"
    "jobs = first\r\n"
    "\r\n"
    "[modules]\r\n"
    "; off - do not load any modules\r\n"
    "enabled = on\r\n"
    "; NAME = on | off: module from the file OMKIT\\MODULES\\NAME.OMM, its files are in the folder OMKIT\\NAME.\r\n"
    "; Line order is load order. Module settings are in its own section [NAME]\r\n"
    "; for example: CREATPRF = on\r\n";

static char text[CFG_MAX + 1];
static struct {
    const char *section, *key, *value;
} entries[CFG_ENTRIES];
static u32 count, bad, first_bad;
#ifdef OMK_DEBUG                            /* the one-shot file (the test scenario's SHOOT.RUN) */
static char once_text[CFG_ONCE_MAX + 1];
static u32 once_from;
static int once_on;
#endif

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int str_ieq(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return lower(*a) == lower(*b);
}

/* Trims spaces on both sides, puts a zero at the end, returns the start */
static char *trim(char *s, char *e)
{
    while (s < e && is_space(*s))
        s++;
    while (e > s && is_space(e[-1]))
        e--;
    *e = 0;
    return s;
}

/* Lines of p[0..len) into the table. only: every key belongs to this section; a header of another one and the keys under it
 * are not understood and not kept (the one-shot file); 0: sections as the text has them. Returns the lines not understood */
static u32 parse(char *p, u32 len, const char *only)
{
    char *end = p + len, *line, *line_end, *eol, *eq, *s;
    const char *section = only ? only : "";
    u32 n = 0, wrong = 0;
    int other = 0;                          /* only: under the header of another section */

    if (len >= 3 && (u8)p[0] == 0xef && (u8)p[1] == 0xbb && (u8)p[2] == 0xbf)
        p += 3;
    for (; p < end; p = eol + 1) {
        n++;
        for (eol = p; eol < end && *eol != '\n'; eol++)
            ;
        line = trim(p, eol);
        for (line_end = line; *line_end; line_end++)
            ;
        if (!*line || *line == ';' || *line == '#')
            continue;
        if (*line == '[') {
            for (s = line + 1; *s && *s != ']'; s++)
                ;
            if (*s == ']') {
                const char *name = trim(line + 1, s);

                if (!only)
                    section = name;
                else
                    other = !str_ieq(name, only);
                if (!other)                         /* the one-shot file: its own section's header only */
                    continue;
            }
        } else {
            for (eq = line; *eq && *eq != '='; eq++)
                ;
            if (*eq == '=' && eq > line && count < CFG_ENTRIES && !other) {
                entries[count].section = section;
                entries[count].value = trim(eq + 1, line_end);
                entries[count].key = trim(line, eq);
                count++;
                continue;
            }
        }
        if (!wrong++ && !bad && !only)
            first_bad = n;
    }
    return wrong;
}

int cfg_load(void)
{
    char path[OMK_PATH_MAX];
    void *fp;

    omk_path(path, 0, CFG_NAME);
    fp = drv_fopen(path, "r");
    u32 len;

    if (!fp) {
        len = sizeof(cfg_template) - 1;
        fp = drv_fopen(path, "wb");
        if (fp) {
            drv_fwrite(cfg_template, 1, len, fp);
            drv_fclose(fp);
        }
        mem_copy(text, cfg_template, len);
        bad = parse(text, len, 0);
        return fp ? 0 : -1;
    }
    len = drv_fread(text, 1, CFG_MAX + 1, fp);  /* one byte more: to tell a full file from a longer one */
    drv_fclose(fp);
    if (len > CFG_MAX) {
        len = CFG_MAX;
        log_printf(OMK_LOG_WARN, "config: OMKIT.INI longer than %u bytes, the rest is ignored", CFG_MAX);
    }
    bad = parse(text, len, 0);
    return 1;
}

#ifdef OMK_DEBUG
/* The one-shot file: read once after OMKIT.INI, its keys are added after the others - the last value wins */
int cfg_once(const char *name, const char *section, u32 *wrong)
{
    char path[OMK_PATH_MAX];
    void *fp;
    u32 len;

    *wrong = 0;
    if (once_on || omk_path(path, 0, name) || !(fp = drv_fopen(path, "r")))
        return 0;
    len = drv_fread(once_text, 1, CFG_ONCE_MAX + 1, fp);   /* one byte more: to tell a full file from a longer one */
    drv_fclose(fp);
    if (len > CFG_ONCE_MAX)
        return -1;                          /* longer than any scenario: not used at all */
    once_on = 1;
    once_from = count;
    *wrong = parse(once_text, len, section);
    return 1;
}

void cfg_once_drop(void)
{
    if (once_on)
        count = once_from;
    once_on = 0;
}

const char *cfg_once_str(const char *section, const char *key)
{
    u32 i;

    for (i = count; once_on && i-- > once_from;)
        if (str_ieq(entries[i].section, section) && str_ieq(entries[i].key, key))
            return entries[i].value;
    return 0;
}
#endif

const char *cfg_str(const char *section, const char *key, const char *def)
{
    u32 i;

    for (i = count; i--;)              /* the last value wins */
        if (str_ieq(entries[i].section, section) && str_ieq(entries[i].key, key))
            return entries[i].value;
    return def;
}

int cfg_int(const char *section, const char *key, int def)
{
    const char *s = cfg_str(section, key, 0);
    u32 v = 0, base = 10, d;
    int neg = 0;

    if (!s || !*s)
        return def;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    if (!*s)
        return def;
    for (; *s; s++) {
        char c = lower(*s);

        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else
            return def;
        v = v * base + d;
    }
    return neg ? -(int)v : (int)v;
}

int cfg_size(const char *section, const char *key, u32 *out)
{
    const char *s = cfg_str(section, key, 0);
    u32 v = 0, digits = 0;

    if (!s)
        return 0;
    for (; *s >= '0' && *s <= '9'; s++, digits++) {
        if (v > 0x0fffffff / 10)
            return -1;
        v = v * 10 + (*s - '0');
    }
    if (!digits)
        return -1;
    if (lower(*s) == 'k' && v <= 0x3fffff) {
        v <<= 10;
        s++;
    } else if (lower(*s) == 'm' && v <= 0xfff) {
        v <<= 20;
        s++;
    }
    if (*s)
        return -1;
    *out = v;
    return 1;
}

int cfg_bool(const char *section, const char *key, int def)
{
    const char *s = cfg_str(section, key, 0);

    if (!s)
        return def;
    if (str_ieq(s, "on") || str_ieq(s, "yes") || str_ieq(s, "true") || str_ieq(s, "1"))
        return 1;
    if (str_ieq(s, "off") || str_ieq(s, "no") || str_ieq(s, "false") || str_ieq(s, "0"))
        return 0;
    return def;
}

/* The sections only a debug build reads. A release build names their keys in one warning and reads none of them (the
 * debug build's keys of the other sections are simply not known to it: "[core] KEY unknown"); a debug build reads them,
 * so the answer there is 0 */
int cfg_debug_section(const char *section)
{
#ifdef OMK_DEBUG
    (void)section;
    return 0;
#else
    static const char *const sections[] = {"debug", "shoot"};
    static const char sets[] = "optics.";   /* [optics.A] ...: the further sets of the optics layer's test schedule */
    u32 i;

    for (i = 0; i < sizeof(sections) / sizeof(sections[0]); i++)
        if (str_ieq(section, sections[i]))
            return 1;
    for (i = 0; sets[i] && lower(section[i]) == sets[i]; i++)
        ;
    return !sets[i];
#endif
}

u32 cfg_count(void)
{
    return count;
}

void cfg_entry(u32 i, const char **section, const char **key, const char **value)
{
    *section = entries[i].section;
    *key = entries[i].key;
    *value = entries[i].value;
}

u32 cfg_bad_lines(u32 *first)
{
    *first = first_bad;
    return bad;
}

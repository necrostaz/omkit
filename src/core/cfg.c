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
    u32 i;

    for (i = 0; i < sizeof(sections) / sizeof(sections[0]); i++)
        if (str_ieq(section, sections[i]))
            return 1;
    return 0;
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

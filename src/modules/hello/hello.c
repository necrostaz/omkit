/* HELLO: OMKit test module. Exercises the loader and the API: relocations (tables of pointers to strings and functions,
 * switch with a jump table, data and bss), log, config [HELLO], memory, store, a file in its own folder, resources,
 * strings from res/strings.txt, on-demand resource res/disk/2_1.dat.
 * Settings: greeting = greeting text; fail = on - module_init returns an error (tests unloading). */
#include "omk_api.h"
#include "res_strings.h"

#define KEY_BOOTS 1
#define KEY_OFFS 2                          /* power-offs seen: written by the power handler, saved by core */
#define RES_TEXT 1                          /* resource 1_1.txt */
#define RES_DISK 2                          /* res/disk/2_1.dat: 5000 bytes, byte i = 7i + 3 */

static const omk_api *api;
static omk_mod *self;
static omk_store *store;
static u32 offs;
static u32 boots;                           /* bss */
static int counter = 40;                    /* data */
static char line[64];                       /* bss */

static int add(int a, int b)
{
    return a + b;
}

static int mul(int a, int b)
{
    return a * b;
}

static int (*const ops[])(int, int) = {add, mul};               /* function pointers in constants */
static const char *const words[] = {"one", "two", "three"};    /* pointers to strings */
static const char **word_ptr = (const char **)&words[2];        /* pointer to a pointer in data */

/* switch with a jump table */
static const char *season(u32 n)
{
    switch (n % 6) {
    case 0:
        return "winter";
    case 1:
        return "spring";
    case 2:
        return "summer";
    case 3:
        return "autumn";
    case 4:
        return "rain";
    default:
        return "snow";
    }
}

static u32 checksum(const char *s)
{
    u32 h = 5381;

    while (*s)
        h = h * 33 + (u8)*s++;
    return h;
}

/* --- events: count in the handler, log from the core task (defer) --- */
static struct {
    u32 checks, allowed, replaced, first, last, s1, pending;
    int hide;
} ev;

static void report(void *arg)
{
    (void)arg;
    ev.pending = 0;
    api->log(self, OMK_LOG_INFO, "pictmode checks %u (replace %u), allowed %u, modes %u..%u, s1 %u", ev.checks, ev.replaced,
             ev.allowed, ev.first, ev.last, ev.s1);
}

static void report_later(void)
{
    if (!ev.pending && !api->defer(self, report, 0))
        ev.pending = 1;
}

static void on_check_replace(omk_ev *e, void *data, void *ctx)
{
    (void)data;
    (void)ctx;
    ev.replaced++;
    api->next(e);                           /* pass-through replace: on down the chain and into the original */
}

static void on_check_filter(omk_ev *e, void *data, void *ctx)
{
    omk_ev_pictmode_check *d = data;

    (void)e;
    (void)ctx;
    if ((int)d->mode == ev.hide)
        d->allowed = 0;
}

static void on_check_done(omk_ev *e, void *data, void *ctx)
{
    omk_ev_pictmode_check *d = data;

    (void)e;
    (void)ctx;
    if (!ev.checks)
        ev.first = d->mode;
    ev.last = d->mode;
    ev.checks++;
    ev.allowed += d->allowed;
    report_later();
}

static void on_s1(omk_ev *e, void *data, void *ctx)
{
    (void)e;
    (void)data;
    (void)ctx;
    ev.s1++;
    report_later();
}

static void register_events(void)
{
    int rc[6];

    ev.hide = api->cfg_int(self, "hide", -1);
    rc[0] = api->on(self, OMK_EV_PICTMODE_CHECK, OMK_PH_REPLACE, 5, on_check_replace, 0);
    rc[1] = api->on(self, OMK_EV_PICTMODE_CHECK, OMK_PH_FILTER_AFTER, 0, on_check_filter, 0);
    rc[2] = api->on(self, OMK_EV_PICTMODE_CHECK, OMK_PH_NOTIFY_AFTER, 0, on_check_done, 0);
    rc[3] = api->on(self, OMK_EV_S1_API, OMK_PH_NOTIFY_BEFORE, 0, on_s1, 0);
    rc[4] = api->on(self, OMK_EV_PICTMODE_CHECK, OMK_PH_FILTER_BEFORE, 0, on_check_filter, 0);  /* not allowed */
    rc[5] = api->on(self, 99, OMK_PH_NOTIFY_BEFORE, 0, on_s1, 0);                                  /* no such event */
    api->log(self, OMK_LOG_INFO, "events: %d %d %d %d, filter_before %d, unknown %d, hide %d", rc[0], rc[1], rc[2], rc[3],
             rc[4], rc[5], ev.hide);
}

/* Power states (API 5): the handler runs in the core task; on power-off only the store in memory is updated, core saves it */
static void on_power(u32 event, void *ctx)
{
    (void)ctx;
    if (event == OMK_POWER_OFF && store) {
        offs++;
        api->store_set(store, KEY_OFFS, &offs, sizeof(offs), OMK_STORE_PERSIST);
    }
    api->log(self, OMK_LOG_INFO, "power event %u, power-offs seen %u", event, offs);
}

int module_init(const omk_api *a, omk_mod *m)
{
    omk_store *st;
    omk_file *f;
    const char *text;
    void *p;
    u32 i, n = 0;
    omk_mem_info mi;

    if (a->version < OMK_API_VERSION || a->size < sizeof(*a))
        return 1;
    api = a;
    self = m;
    OMK_MODULE_KIND(api, self);
    st = store = api->store_open(self, 64);
    if (st) {
        api->store_get(st, KEY_OFFS, &offs, sizeof(offs));
        api->store_get(st, KEY_BOOTS, &boots, sizeof(boots));
        boots++;
        api->store_set(st, KEY_BOOTS, &boots, sizeof(boots), OMK_STORE_PERSIST);
    }
    api->log(self, OMK_LOG_INFO, "%s, boot %u", api->cfg_str(self, "greeting", "hello"), boots);
    api->log(self, OMK_LOG_INFO, "words %s %s %s, last %s, ops %d %d, season %s, bss %u", words[0], words[1], words[2],
             *word_ptr, ops[0](counter, 2), ops[1](counter, 2), season(boots), (u32)line[0]);

    p = api->alloc(self, 1000);
    api->mem_info(self, &mi);
    api->log(self, OMK_LOG_INFO, "alloc 1000 at %x, used %u", (u32)(unsigned long)p, mi.used);
    api->free(self, p);
    api->free(self, (char *)p + 16);        /* not the start of a block: core refuses and logs it */

    text = api->res(self, 1, RES_TEXT, &n);
    for (i = 0; text && i < n && i < sizeof(line) - 1 && text[i] != '\r' && text[i] != '\n'; i++)
        line[i] = text[i];
    line[i] = 0;
    api->log(self, OMK_LOG_INFO, "resource 1/%u: %u bytes, '%s', hash %x", RES_TEXT, n, line, checksum(line));

    /* strings: English ASCII, ids from res_strings.h; identical texts are stored once */
    api->log(self, OMK_LOG_INFO, "strings %u: '%s', '%s', same stored once %s, empty '%s', missing '%s'", STR_COUNT,
             api->str(self, STR_TITLE), api->str(self, STR_GREETING),
             api->str(self, STR_SAME) == api->str(self, STR_TITLE) ? "yes" : "no", api->str(self, STR_EMPTY),
             api->str(self, 999));

    /* on-demand resource: not in memory, a chunk is read from the card */
    {
        u8 part[8];
        u32 size = 0, got;
        const void *mem = api->res(self, RES_DISK, 1, &size);

        got = api->res_read(self, RES_DISK, 1, 4000, part, sizeof(part));
        api->log(self, OMK_LOG_INFO, "disk resource 2/1: %u bytes, in memory %s, at 4000: %u bytes %x %x %x", size,
                 mem ? "yes" : "no", got, part[0], part[1], part[7]);
        got = api->res_read(self, RES_DISK, 1, size - 3, part, sizeof(part));
        api->log(self, OMK_LOG_INFO, "disk resource tail: %u bytes", got);
    }

    f = api->file_open(self, "HELLO.TXT", "w");
    if (f) {
        static const char msg[] = "HELLO module was here\r\n";

        api->file_write(f, msg, sizeof(msg) - 1);
        api->file_close(f);
    }
    api->log(self, OMK_LOG_INFO, "file HELLO.TXT %s", f ? "written" : "not written (no folder OMKIT\\HELLO?)");
    api->file_open(self, "..\\X", "r");     /* invalid name: refusal in the log */

    api->log(self, OMK_LOG_INFO, "power handler %d, power-offs seen %u", api->on_power(self, on_power, 0), offs);
    register_events();

    if (api->cfg_bool(self, "fail", 0)) {
        api->log(self, OMK_LOG_WARN, "fail = on, returning an error");
        return 7;
    }
    return 0;
}

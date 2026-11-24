/* The test scenario's state machine (src/core/shoot.c) against a simulated camera: the order of the key presses, every
 * press released on every way out (done, pictures not made, stop, power-off), the watchdog of a stuck half-press, the
 * steps' parsing and presets. Run by tests/host/run.sh: test_shoot <empty dir> [keys|wifi] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "omk_core.h"
#include "omk_log.h"
#include "omk_shoot.h"
#include "host_drv.h"

static int failed;

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);             \
            failed++;                                                           \
        }                                                                       \
    } while (0)

/* The simulated camera: the key image, the application state, the picture counter */
static struct {
    int down[2];                            /* the keys as the camera sees them */
    u32 half_since, release_ms;             /* when the half-press went down; when the full press went up (0 - not yet) */
    int no_half;                            /* never shows the half-press state */
    int makes;                              /* a full press makes a picture */
    u32 stuck_ms;                           /* the half-press state stays this long after the release (until a second one) */
    u32 path, presses, releases, extra_releases;
    char trace[512];
    u32 looks[8], nlooks, profs[8], nprofs, look_count;
    int look_on;
    int usb;                                /* in a mode of USB (MTP): the application state is not readable */
} cam;

void modules_power(u32 event);              /* power.c's call to the modules (module.c is not in this program) */

void modules_power(u32 event)
{
    (void)event;
}

static u32 now_ms(void)
{
    return omk_ms();
}

int drv_shutter(u32 key, int down, u32 path)
{
    char *t = cam.trace + strlen(cam.trace);

    if (key > 1)
        return -1;
    cam.path = path;
    if (strlen(cam.trace) + 4 < sizeof(cam.trace))
        sprintf(t, "%c%d ", key ? 'F' : 'H', down ? 1 : 0);
    if (down) {
        cam.presses++;
        if (key == 0)
            cam.half_since = now_ms();
    } else {
        cam.releases++;
        if (!cam.down[key])
            cam.extra_releases++;
        if (key == 1 && cam.down[1])
            cam.release_ms = now_ms();
    }
    cam.down[key] = down != 0;
    return 0;
}

/* K2: half-press state 100 ms after the half-press went down; and for stuck_ms after its release */
static u32 stuck_until;

int drv_ui_probe(u32 *app, u32 *keys)
{
    u32 now = now_ms();

    *keys = 0;
    if (cam.usb)
        return -1;
    if (!cam.no_half && ((cam.down[0] && now - cam.half_since >= 100) || (stuck_until && now < stuck_until))) {
        *app = 0x5101;
        return DRV_UI_HALF;
    }
    *app = 0x5100;
    return 0;
}

void drv_look_stats(drv_look_info *st)
{
    memset(st, 0, sizeof(*st));
    st->count = cam.look_count;
    st->active = cam.look_on;
}

int drv_look_choose(int on, u32 index)
{
    if (!cam.look_count)
        return -1;
    if (on && index >= cam.look_count)
        return -1;
    cam.look_on = on;
    if (cam.nlooks < 8)
        cam.looks[cam.nlooks++] = on ? index : 0xff;
    return 0;
}

int drv_look_find(const char *name)
{
    return !strcmp(name, "Tri-X") ? 1 : -1;
}

void drv_prof_choose(u32 prof)
{
    if (cam.nprofs < 8)
        cam.profs[cam.nprofs++] = prof;
}

/* The camera between two passes of the core loop: the picture made 400 ms after the full press's release */
static void camera_step(void)
{
    u32 now = now_ms();

    if (cam.release_ms && now - cam.release_ms >= 400) {
        if (cam.makes)
            host_prof_takes++;
        cam.release_ms = 0;
        if (cam.stuck_ms)
            stuck_until = now + cam.stuck_ms;
    }
    if (stuck_until && !cam.down[0] && cam.releases >= 2 && cam.extra_releases)
        stuck_until = 0;                    /* the second release cleared it */
}

/* Runs the core loop's calls for up to ms; stops early when the scenario is not running and stop_idle */
static void run(u32 ms, int stop_idle)
{
    u32 end = now_ms() + ms, next = now_ms();
    shoot_info si;

    while ((s32)(now_ms() - end) < 0) {
        camera_step();
        if ((s32)(now_ms() - next) >= 0) {
            u32 d = shoot_service(now_ms(), 250);

            CHECK(d >= 20 && d <= 250);
            next = now_ms() + d;
        }
        shoot_status(&si);
        if (stop_idle && si.state != SHOOT_RUNNING)
            return;
        drv_sleep(10);
    }
}

static void reset_cam(void)
{
    memset(&cam, 0, sizeof(cam));
    cam.makes = 1;
    cam.look_count = 3;
    stuck_until = 0;
}

static int all_released(void)
{
    return !cam.down[0] && !cam.down[1];
}

static void test_parse(void)
{
    CHECK(shoot_parse("look:1, look:Tri-X , prof:c3, prof:m1, stock, keep") == 0);
    CHECK(shoot_parse("") == 0);
    CHECK(shoot_parse(0) == 0);
    CHECK(shoot_parse("look:0") == -1);
    CHECK(shoot_parse("look:") == -1);
    CHECK(shoot_parse("prof:x1") == -1);
    CHECK(shoot_parse("prof:c16") == -1);
    CHECK(shoot_parse("nothing") == -1);
    CHECK(shoot_parse("look:a-name-much-longer-than-the-buffer-of-a-step") == -1);
    CHECK(shoot_parse("keep,keep,keep,keep,keep,keep,keep,keep,keep,keep,keep,keep,keep,keep,keep,keep") == 0);
    CHECK(shoot_parse("keep,keep,keep,keep,keep,keep,keep,keep,keep,keep,keep,keep,keep,keep,keep,keep,keep") == -1);
}

/* Two steps of two pictures: the presets in order, each picture H1 F1 F0 H0 */
static void test_done(int wifi)
{
    shoot_info si;

    reset_cam();
    CHECK(shoot_start("look:1, look:Tri-X", 1000) == 0);
    CHECK(shoot_start(0, 0) == -2);         /* one at a time */
    run(60000, 1);
    shoot_status(&si);
    CHECK(si.state == SHOOT_DONE);
    CHECK(si.taken == 4 && si.failed == 0 && si.total == 4);
    CHECK(!strcmp(cam.trace, "H1 F1 F0 H0 H1 F1 F0 H0 H1 F1 F0 H0 H1 F1 F0 H0 "));
    CHECK(cam.nlooks == 2 && cam.looks[0] == 0 && cam.looks[1] == 1);
    CHECK(cam.path == (u32)(wifi ? DRV_SHUTTER_WIFI : DRV_SHUTTER_KEYS));
    CHECK(all_released() && cam.extra_releases == 0);
}

/* No picture ever: three in a row not made - the scenario stops, nothing stays pressed */
static void test_no_pictures(void)
{
    shoot_info si;

    reset_cam();
    cam.makes = 0;
    CHECK(shoot_start("keep", 0) == 0);
    run(120000, 1);
    shoot_status(&si);
    CHECK(si.state == SHOOT_DONE);          /* shots = 2: both not made (two in a row), then the end of the steps */
    CHECK(si.taken == 0 && si.failed == 2);
    CHECK(all_released());
    reset_cam();
    cam.makes = 0;
    CHECK(shoot_start("keep, keep", 0) == 0);
    run(200000, 1);
    shoot_status(&si);
    CHECK(si.state == SHOOT_FAILED && si.failed == 3);   /* FAILS_MAX in a row */
    CHECK(all_released());
}

/* A stop while the half-press is down, a power-off while the full press is down: both released, the full one first */
static void test_ways_out(void)
{
    shoot_info si;
    u32 i;

    reset_cam();
    CHECK(shoot_start("keep", 0) == 0);
    for (i = 0; i < 2000 && !cam.down[0]; i++)
        run(10, 0);
    CHECK(cam.down[0]);
    shoot_stop();
    shoot_status(&si);
    CHECK(si.state == SHOOT_STOPPED);
    CHECK(all_released());
    reset_cam();
    CHECK(shoot_start("keep", 0) == 0);
    for (i = 0; i < 3000 && !cam.down[1]; i++)
        run(10, 0);
    CHECK(cam.down[0] && cam.down[1]);
    shoot_power_lost();                     /* a power-off began (the cable out in the PC mode, cancelled by the lever) */
    shoot_status(&si);
    CHECK(si.state == SHOOT_RUNNING && si.waiting);
    CHECK(all_released());
    CHECK(strstr(cam.trace, "F1 F0 H0 ") != 0);
    shoot_power_lost();                     /* again while it lasts: nothing more sent */
    CHECK(cam.releases == 2);
    cam.usb = 1;                            /* not shooting for a while, then shooting again: the picture taken again */
    run(5000, 1);
    shoot_status(&si);
    CHECK(si.state == SHOOT_RUNNING && si.waiting && cam.presses == 2);
    cam.usb = 0;
    run(60000, 1);
    shoot_status(&si);
    CHECK(si.state == SHOOT_DONE && si.taken == 2 && si.failed == 0 && all_released());
    shoot_power_lost();                     /* nothing running: nothing sent */
    CHECK(cam.releases == 6);
}

/* The half-press state stays after the release: the watchdog releases once more, the picture counts */
static void test_stuck(void)
{
    shoot_info si;

    reset_cam();
    cam.stuck_ms = 60000;
    CHECK(shoot_start("keep", 0) == 0);
    run(60000, 1);
    shoot_status(&si);
    CHECK(si.taken == 2 && si.state == SHOOT_DONE);
    CHECK(cam.extra_releases >= 1);
    CHECK(all_released());
}

/* No half-press state ever (focus not found, say): the full press after `half` anyway */
static void test_no_half(void)
{
    shoot_info si;

    reset_cam();
    cam.no_half = 1;
    CHECK(shoot_start("keep", 0) == 0);
    run(60000, 1);
    shoot_status(&si);
    CHECK(si.taken == 2 && si.state == SHOOT_DONE);
    CHECK(all_released());
}

/* Presets that are not there: a profile step without the profiles' owner, a look entry out of range - their pictures are
 * not taken, no key is pressed for them */
static void test_bad_presets(void)
{
    shoot_info si;

    reset_cam();
    CHECK(shoot_start("prof:c1, look:9", 0) == 0);
    run(60000, 1);
    shoot_status(&si);
    CHECK(si.taken == 0 && si.failed == 4 && si.state == SHOOT_DONE);
    CHECK(cam.presses == 0 && cam.nprofs == 0);
    reset_cam();
    CHECK(shoot_start("stock", 0) == 0);    /* the look off */
    run(60000, 1);
    shoot_status(&si);
    CHECK(si.taken == 2 && cam.nlooks == 1 && cam.looks[0] == 0xff);
}

/* A start while the camera does not shoot (USB): nothing is pressed until it does, then the scenario runs; and the time
 * limit of that wait (arm_wait = 20000 in the test's OMKIT.INI) */
static void test_armed(void)
{
    shoot_info si;

    reset_cam();
    cam.usb = 1;
    CHECK(shoot_start("keep", 0) == 0);
    run(15000, 1);
    shoot_status(&si);
    CHECK(si.state == SHOOT_RUNNING && si.waiting && cam.presses == 0);
    cam.usb = 0;
    run(1000, 1);
    shoot_status(&si);
    CHECK(si.waiting && cam.presses == 0);  /* ARM_SETTLE: two seconds of shooting first */
    run(60000, 1);
    shoot_status(&si);
    CHECK(si.state == SHOOT_DONE && !si.waiting && si.taken == 2 && all_released());
    reset_cam();
    cam.usb = 1;
    CHECK(shoot_start("keep", 0) == 0);
    run(30000, 1);
    shoot_status(&si);
    CHECK(si.state == SHOOT_FAILED && cam.presses == 0);
}

static int exists(const char *name)
{
    char p[512];
    FILE *f;

    snprintf(p, sizeof(p), "%s/%s", host_dir, name);
    f = fopen(p, "rb");
    if (f)
        fclose(f);
    return f != 0;
}

/* SHOOT.RUN of the OMKit folder: its keys over those of OMKIT.INI, removed before the scenario starts; a header of another
 * section is a line not understood and its keys are not used */
static void test_once(void)
{
    shoot_info si;
    char p[512];
    FILE *f;

    reset_cam();
    snprintf(p, sizeof(p), "%s/SHOOT.RUN", host_dir);
    f = fopen(p, "wb");
    fprintf(f, "; one picture\r\n[shoot]\r\nsteps = keep\r\nshots = 1\r\nstart_delay = 0\r\n[core]\r\nlog = off\r\n");
    fclose(f);
    shoot_setup(now_ms());
    CHECK(!exists("SHOOT.RUN"));
    CHECK(!str_ieq(cfg_str("core", "log", "none"), "off") && !cfg_str("shoot", "log", 0));
    CHECK(str_ieq(cfg_str("shoot", "shots", ""), "1"));
    run(60000, 1);
    shoot_status(&si);
    CHECK(si.state == SHOOT_DONE && si.taken == 1 && si.total == 1);
    shoot_setup(now_ms());                  /* once per start: nothing more */
    shoot_status(&si);
    CHECK(si.state == SHOOT_DONE && si.taken == 1);
}

int main(int argc, char **argv)
{
    int wifi = argc > 2 && !strcmp(argv[2], "wifi");
    FILE *f;
    char path[512];

    host_dir = argc > 1 ? argv[1] : ".";
    snprintf(path, sizeof(path), "%s/OMKIT.INI", host_dir);
    f = fopen(path, "wb");
    if (!f)
        return 2;
    fprintf(f, "[shoot]\r\nshots = 2\r\ninterval = 1000\r\nsettle = 500\r\nhalf = 1000\r\nhold = 150\r\nwrite = 5000\r\n"
               "arm_wait = 20000\r\ntrigger = %s\r\n", wifi ? "wifi" : "keys");
    fclose(f);
    mem_init();
    CHECK(cfg_load() == 1);
    log_setup(0, OMK_LOG_DEBUG);
    test_parse();
    test_done(wifi);
    if (!wifi) {
        test_no_pictures();
        test_ways_out();
        test_stuck();
        test_no_half();
        test_bad_presets();
        test_armed();
        test_once();
    }
    if (failed)
        fprintf(stderr, "%d checks failed\n", failed);
    else
        printf("ok\n");
    return failed != 0;
}

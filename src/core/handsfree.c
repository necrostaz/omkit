/* A test scenario without hands: OMKIT\SHOOT.RUN with hands_free = on (docs/config.md, [shoot]).
 *
 * The usual way to test a device on a bench without a person is a fixture that switches its cable; here the camera
 * itself keeps the cable as charge only - the firmware's own USB choice "charge only" (it shoots, the data link stays
 * down) - for the one start that SHOOT.RUN asks for, and comes back for the computer by a restart. So:
 *  - hold: the driver's hook on the firmware's choice of the USB mode turns every choice into "charge only" while the
 *    hold is on; the firmware does the rest (the power from USB, the shooting application). The camera's USB setting is
 *    not changed. If the camera shows its USB dialog, core answers it with OK and the hook takes the answer;
 *  - the end - the scenario done, failed or stopped, or its time limits, or the application stuck: the result goes to
 *    OMKIT\SHOOT.END (a fixed length, rewritten in place), the hold is let go, and core restarts the camera past the
 *    application (a power-off job of the firmware, as its factory reset makes one), the watchdog as the last resort;
 *  - the next start: SHOOT.END asks once (connect=mtp) for the MTP mode, so a camera whose USB mode is "ask" connects
 *    to the computer without a hand; one whose USB mode is MTP does so by itself.
 * Every failure on the way lets the hold go (fail-safe): the next start is an ordinary one. The flag is one-shot -
 * SHOOT.RUN is removed before anything happens - and is not taken from OMKIT.INI. */
#include "omk_core.h"
#include "omk_handsfree.h"
#include "omk_log.h"
#include "omk_power.h"
#include "omk_shoot.h"
#include "omk_taskdump.h"

#define SEC "shoot"
#define START_MS 120000                     /* the scenario has not started this long after the hold: given up */
#define MISSED_MS 3000                      /* connected to the computer (MTP, Storage) this long: the hold came too late */
#define OK_EVERY_MS 3000                    /* the USB dialog: OK pressed again after this, ... */
#define OK_TRIES 5                          /* ... at most this many times */
#define OK_HOLD_MS 150
#define FAILED_SETTLE_MS 3000               /* a scenario that cannot start: the end once the camera shoots this long (out of
                                               the USB dialog, as at the end of every scenario: the restart known to work) */
#define STUCK_RESTART_MS 30000              /* the application stuck: this long after its task dump, the scenario ends */
#define CARD_WAIT_MS 10000                  /* the camera writes a picture: the restart waits at most this long */
#define OFF_WAIT_MS 1000                    /* the power-off job: its hook within this, ... */
#define OFF_POSTS 2                         /* ... posted at most this many times */
#define FLASH_WAIT_MS 3000                  /* power-off begun: the settings' write (K3) begins within this */
#define CARD_OFF_WAIT_MS 20000              /* ... and the card-off gate comes within this (a settings write may take 10 s) */
#define CUT_WAIT_MS 10000                   /* the card unmounted: the power goes within this */
#define MTP_WINDOW_MS 60000                 /* connect=mtp: the hold on MTP at most this long after the start */
#define RESET_CAUSE 0x4f4d4b48              /* "OMKH": the cause of the watchdog reset, in (0x8ff000b0) for the next start */
#define SESSION_MAX 16

enum { HF_OFF, HF_HELD, HF_ENDING, HF_RESTART, HF_STAYS, HF_MTP };
enum { R_POST, R_OFF, R_FLASH, R_CARD, R_CUT };

static struct {
    u32 state, t0, since, ok_t, oks, stage, stage_t, posts, offs0, cards0, missed_t, last_hits, shoot_t;
    int started, stuck, restart;
    const char *why;                        /* core's own reason to end the scenario */
    char session[SESSION_MAX + 1];
} hf;

int hf_supported(void)
{
    return 1;
}

int hf_active(void)
{
    return hf.state == HF_HELD;
}

static void copy(char *out, const char *in, u32 max)
{
    u32 i;

    for (i = 0; in && in[i] && i < max; i++)
        out[i] = in[i] >= ' ' && in[i] <= '~' && in[i] != '=' ? in[i] : '_';
    out[i] = 0;
}

static const char *choice_name(u32 c)
{
    switch (c) {
    case DRV_USB_DIALOG:
        return "the dialog";
    case DRV_USB_STORAGE:
        return "Storage";
    case DRV_USB_MTP:
        return "MTP";
    case DRV_USB_CHARGE:
        return "charge only";
    }
    return "other";
}

/* The camera's USB state, for the log: when core starts, the firmware has often chosen already */
static void log_usb(const char *when)
{
    drv_usb_info u;

    drv_usb_probe(&u);
    log_printf(OMK_LOG_INFO, "hands-free: %s: USB choice %u (%s), start factor %u, TPC state %u, app %x, hook %u times",
               when, u.choice, choice_name(u.choice), u.factor, u.link, u.app, drv_usb_hold_hits());
}

/* ---- OMKIT\SHOOT.END: key=value lines, spaces to HF_RESULT_SIZE ---- */
static u32 put_str(char *b, u32 at, const char *s)
{
    while (*s && at < HF_RESULT_SIZE - 2)
        b[at++] = *s++;
    return at;
}

static u32 put_num(char *b, u32 at, u32 v)
{
    char t[11];
    u32 n = 0;

    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n && at < HF_RESULT_SIZE - 2)
        b[at++] = t[--n];
    return at;
}

static int write_result(const char *result, const char *why, const shoot_info *si, int restart)
{
    char b[HF_RESULT_SIZE], path[OMK_PATH_MAX];
    u32 at, i;
    void *fp;
    int rc;

    at = put_str(b, 0, "session=");
    at = put_str(b, at, hf.session);
    at = put_str(b, put_str(b, at, "\r\nrun="), core_run());
    at = put_str(b, put_str(b, at, "\r\nbuild="), CORE_BUILD);
    at = put_str(b, put_str(b, at, "\r\nresult="), result);
    at = put_str(b, put_str(b, at, "\r\nwhy="), why);
    at = put_num(b, put_str(b, at, "\r\ntaken="), si->taken);
    at = put_num(b, put_str(b, at, "\r\ntotal="), si->total);
    at = put_num(b, put_str(b, at, "\r\nfailed="), si->failed);
    at = put_num(b, put_str(b, at, "\r\nusb_hook="), drv_usb_hold_hits());
    at = put_str(b, put_str(b, at, "\r\nconnect="), restart ? "mtp " : "none");
    at = put_str(b, at, "\r\n");
    for (i = at; i < HF_RESULT_SIZE - 2; i++)
        b[i] = ' ';
    b[HF_RESULT_SIZE - 2] = '\r';
    b[HF_RESULT_SIZE - 1] = '\n';
    if (omk_path(path, 0, HF_RESULT_FILE))
        return -1;
    core_card_write(1, path);
    fp = drv_fopen(path, "r+b");            /* the same length in place: no change of the folder */
    if (!fp)
        fp = drv_fopen(path, "wb");
    rc = !fp ? -1 : drv_fwrite(b, 1, HF_RESULT_SIZE, fp) != HF_RESULT_SIZE ? -2 : 0;
    if (fp && drv_fclose(fp) && !rc)
        rc = -3;
    core_card_write(0, path);
    return rc;
}

static int starts(const char *b, const char *p)
{
    while (*p)
        if (*b++ != *p++)
            return 0;
    return 1;
}

/* The start after a scenario without hands: SHOOT.END's connect=mtp, taken once (rewritten to "none" first) */
static int take_connect(void)
{
    char b[HF_RESULT_SIZE], path[OMK_PATH_MAX];
    u32 n, i;
    void *fp;
    int found = -1;

    if (omk_path(path, 0, HF_RESULT_FILE) || !(fp = drv_fopen(path, "r+b")))
        return 0;
    n = drv_fread(b, 1, HF_RESULT_SIZE, fp);
    for (i = 0; i + 13 <= n; i++)
        if (b[i] == '\n' && starts(b + i + 1, "connect=mtp ")) {
            found = (int)i + 9;
            break;
        }
    if (found >= 0 && !drv_fseek(fp, (u32)found) && drv_ftell(fp) == (u32)found) {
        b[0] = 'n', b[1] = 'o', b[2] = 'n', b[3] = 'e';
        if (drv_fwrite(b, 1, 4, fp) != 4)
            found = -2;
    } else if (found >= 0) {
        found = -2;
    }
    if (drv_fclose(fp) && found >= 0)
        found = -2;
    if (found == -2) {
        log_printf(OMK_LOG_ERROR, "hands-free: %s asks for MTP but cannot be changed: not taken", HF_RESULT_FILE);
        return 0;
    }
    return found >= 0;
}

/* ---- setup ---- */
void hf_setup(int once)
{
    const char *v = cfg_once_str(SEC, "hands_free");
    int rc;

    if (cfg_str(SEC, "hands_free", 0) && !v)
        log_printf(OMK_LOG_WARN, "config: [shoot] hands_free is taken only from %s, ignored", "SHOOT.RUN");
    if (!once || !v || !str_ieq(v, "on")) {
        if (v && !str_ieq(v, "off"))
            log_printf(OMK_LOG_WARN, "config: [shoot] hands_free '%s' unknown (on, off)", v);
        if (take_connect()) {
            if ((rc = drv_usb_hold_install())) {
                log_printf(OMK_LOG_ERROR, "hands-free: the USB hook not installed (code %d): no MTP without a hand", rc);
                return;
            }
            drv_usb_hold(DRV_USB_MTP);
            hf.state = HF_MTP;
            hf.t0 = hf.ok_t = omk_ms();
            log_usb("after the scenario, MTP asked for once");
        }
        return;
    }
    copy(hf.session, cfg_once_str(SEC, "session"), SESSION_MAX);
    hf.t0 = hf.ok_t = omk_ms();
    rc = drv_usb_hold_install();
    if (rc) {
        log_printf(OMK_LOG_ERROR, "hands-free: the USB hook not installed (code %d): USB is not held, the scenario ends at "
                   "its start", rc);
        hf.why = "the USB hook could not be installed";
        hf.state = HF_HELD;
        return;
    }
    drv_usb_hold(DRV_USB_CHARGE);
    hf.state = HF_HELD;
    log_printf(OMK_LOG_INFO, "hands-free: USB held as charge only until the scenario ends (session %s)",
               hf.session[0] ? hf.session : "-");
    log_usb("at the start");
}

/* ---- the USB dialog: OK, which the hook turns into the mode held ---- */
static void answer_dialog(u32 now)
{
    drv_usb_info u;

    drv_usb_probe(&u);
    if (u.choice != DRV_USB_DIALOG || u.app != 0xffffffff || hf.oks >= OK_TRIES || now - hf.ok_t < OK_EVERY_MS)
        return;
    hf.ok_t = now;
    hf.oks++;
    log_printf(OMK_LOG_INFO, "hands-free: the USB dialog: OK pressed (%u of %u), the hook makes it %s", hf.oks, OK_TRIES,
               choice_name(drv_usb_hold_get()));
    drv_ok_key(1);
    drv_sleep(OK_HOLD_MS);
    drv_ok_key(0);
}

/* ---- the end and the restart ---- */
static void end(u32 now, int restart)
{
    shoot_info si;
    const char *result;
    int rc;

    shoot_status(&si);
    result = si.state == SHOOT_DONE ? "done" : si.state == SHOOT_STOPPED ? "stopped" : "failed";
    drv_usb_hold(0);                        /* first: whatever happens next, the next choice is the firmware's own */
    rc = write_result(result, hf.why ? hf.why : si.why, &si, restart);
    log_printf(rc ? OMK_LOG_ERROR : OMK_LOG_INFO, "hands-free: the scenario %s (%s), USB let go, %s %s (%d); %s", result,
               hf.why ? hf.why : si.why, HF_RESULT_FILE, rc ? "NOT written" : "written", rc,
               restart ? "restarting the camera" : "the camera stays as it is (connected to the computer)");
    log_usb("at the end");
    hf.restart = restart;
    hf.state = restart ? HF_RESTART : HF_STAYS;
    hf.stage = R_POST;
    hf.stage_t = now;
    hf.posts = 0;
    if (restart)
        log_flush();
}

/* The last resort: the watchdog. The RAM log has the line; the card is left alone (its task may be the one that hangs) */
static void reset(const char *why)
{
    log_printf(OMK_LOG_ERROR, "hands-free: %s: reset by the watchdog (cause %08x), nothing more saved", why, RESET_CAUSE);
    hf.state = HF_STAYS;                    /* drv_reset does not return on the camera */
    drv_reset(RESET_CAUSE);
}

static void restart_service(u32 now)
{
    u32 dt = now - hf.stage_t, w[2];
    int rc;

    switch (hf.stage) {
    case R_POST:
        if (drv_card_busy() && dt < CARD_WAIT_MS)
            return;                         /* a picture being written: after it */
        hf.offs0 = power_hook_count(POWER_HOOK_OFF);
        hf.cards0 = power_hook_count(POWER_HOOK_CARD_OFF);
        rc = drv_restart_hfw();
        hf.posts++;
        log_printf(rc ? OMK_LOG_WARN : OMK_LOG_INFO, "hands-free: restart: the power-off job %s (%d), try %u of %u",
                   rc ? "NOT posted" : "posted", rc, hf.posts, OFF_POSTS);
        hf.stage = R_OFF;
        hf.stage_t = now;
        return;
    case R_OFF:
        if (power_hook_count(POWER_HOOK_OFF) != hf.offs0) {
            hf.stage = R_FLASH;
            hf.stage_t = now;
            return;
        }
        if (dt < OFF_WAIT_MS)
            return;
        if (hf.posts < OFF_POSTS) {
            hf.stage = R_POST;
            return;
        }
        reset("the power-off job did not run");
        return;
    case R_FLASH:
        drv_power_probe(w, 2);
        if (w[1] == 1 || power_hook_count(POWER_HOOK_CARD_OFF) != hf.cards0) {
            hf.stage = R_CARD;
            hf.stage_t = now;
        } else if (dt >= FLASH_WAIT_MS) {
            reset("the power-off began but its settings write did not");
        }
        return;
    case R_CARD:
        if (power_hook_count(POWER_HOOK_CARD_OFF) != hf.cards0) {
            hf.stage = R_CUT;
            hf.stage_t = now;
        } else if (dt >= CARD_OFF_WAIT_MS) {
            reset("the power-off stopped before the card was unmounted");
        }
        return;
    case R_CUT:
        if (dt >= CUT_WAIT_MS)
            reset("the card is unmounted but the power stays");
        return;
    }
}

static void held_service(u32 now)
{
    shoot_info si;
    drv_usb_info u;
    u32 stuck;

    shoot_status(&si);
    if (si.state == SHOOT_RUNNING)
        hf.started = 1;
    if (!hf.why)
        answer_dialog(now);
    if (drv_usb_hold_hits() != hf.last_hits) {  /* before the start too: the hook's work is seen apart from the scenario's */
        hf.last_hits = drv_usb_hold_hits();
        log_usb("the hook took a USB choice");
    }
    if (hf.why && hf.started) {             /* the hook is not there: the scenario ends at once */
        shoot_abort(hf.why);
        end(now, 0);
        return;
    }
    if (!hf.started) {
        drv_usb_probe(&u);
        if (u.app == 0xffffffff)            /* the USB dialog or the PC mode: not yet shooting */
            hf.shoot_t = 0;
        else if (!hf.shoot_t)
            hf.shoot_t = now | 1;
        if ((si.state != SHOOT_IDLE && hf.shoot_t && now - hf.shoot_t >= FAILED_SETTLE_MS) || now - hf.t0 >= START_MS) {
            hf.why = si.state != SHOOT_IDLE ? si.why : "the scenario did not start (the modules, or a step not understood)";
            end(now, 1);
        }
        return;
    }
    if (si.state != SHOOT_RUNNING) {
        end(now, 1);
        return;
    }
    drv_usb_probe(&u);
    if (u.app == 0xffffffff && (u.choice == DRV_USB_MTP || u.choice == DRV_USB_STORAGE)) {
        if (!hf.missed_t)
            hf.missed_t = now | 1;
        else if (now - hf.missed_t >= MISSED_MS) {
            log_usb("connected to the computer");
            hf.why = "the camera connected to the computer before the hold (its USB mode is not \"ask\"?)";
            shoot_abort(hf.why);
            end(now, 0);
            return;
        }
    } else {
        hf.missed_t = 0;
    }
    stuck = taskdump_stuck_since();
    if (stuck && (s32)(now - stuck) >= STUCK_RESTART_MS) {     /* signed: stuck may be now | 1 */
        log_printf(OMK_LOG_ERROR, "hands-free: the application stuck for %u s after its task dump", (now - stuck) / 1000);
        hf.why = "the application stuck";
        shoot_abort(hf.why);
        end(now, 1);
    }
}

void hf_service(u32 now)
{
    switch (hf.state) {
    case HF_HELD:
        if (power_running())                /* a power-off or sleep: the scenario pauses, so does this */
            held_service(now);
        return;
    case HF_RESTART:
        restart_service(now);
        return;
    case HF_MTP:
        if (drv_usb_hold_hits() || now - hf.t0 >= MTP_WINDOW_MS) {
            drv_usb_hold(0);
            log_usb(drv_usb_hold_hits() ? "MTP chosen by the hook, USB let go" : "no USB choice to make, USB let go");
            hf.state = HF_OFF;
            return;
        }
        if (power_running())
            answer_dialog(now);
        return;
    }
}

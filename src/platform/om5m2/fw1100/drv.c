/* OM-5 Mark II driver, firmware 1.1. Firmware addresses live only here, in thunks.s and in fw.ld. */
#include "omk_drv.h"
#include "omk_ev.h"
#include "omk_power.h"

#define CRASH_BASE 0x62ad5580
#define CRASH_STEP 0x40
#define BOOT_COUNT 0xa26e13fc
#define BOOT_EVENTS 0xa26e1400
#define BOOT_EVENT_SIZE 24
#define BOOT_NAME_LEN 20
#define UNCACHED DRV_UNCACHED
#define ARENA_LO 0xa3470000                 /* resource gap: the arena never starts below this */
#define ARENA_HI 0xa3780000
#define ARENA_TAIL 0x20100                  /* the region's end is not the arena: the debug service's memory 0xa375ff00..
                                               0xa377ff80 (ptp.c, drv_keep_area) and the optics layer's crumbs
                                               0xa377ff80..0xa3780000; a reset must not find them overwritten by a block */
#define GUARD_LO 0x60000200                 /* DbgSup protected range */
#define GUARD_HI 0x60cbe800
#define JMP_D32 0xdc
#define JMP_LEN 5
#define ICACHE_LINE 16

/* First bytes of the functions from fw.ld: the loader does not change them */
static const struct {
    u32 addr;
    u8 bytes[8];
} fingerprint[] = {
    {0x606aa3bd, {0x00, 0xcd, 0xb3, 0x13, 0xf0, 0x2c, 0xdf, 0x00}},
    {0x606aa54e, {0x47, 0x04, 0x87, 0x82, 0x93, 0xdd, 0x11, 0xeb}},
    {0x606aa444, {0x47, 0x04, 0x87, 0x82, 0x93, 0xdd, 0x1b, 0xec}},
    {0x60051a2b, {0x08, 0xdd, 0xcb, 0xf9, 0xfd, 0xff, 0xb0, 0x10}},
    {0x602f4c5b, {0x1c, 0xcd, 0x08, 0x00, 0xe0, 0x14, 0xdf, 0x00}},
    {0x6006e645, {0xfc, 0xa5, 0xfc, 0x06, 0xf0, 0x8f, 0x82, 0xfc}},
    {0x6006e68e, {0x82, 0xfc, 0xa4, 0x40, 0x00, 0xf0, 0x8f, 0xa0}},
    {0x60023818, {0xfa, 0xc8, 0x80, 0x00, 0xc5, 0x05, 0x00, 0xca}},
    {0x6002384a, {0xf1, 0xd0, 0xfc, 0xc8, 0x80, 0xe3, 0xa8, 0xa1}},
    {0x606aa3c6, {0x93, 0xdd, 0x9d, 0xec, 0x00, 0x00, 0x00, 0x08}},   /* fclose */
    {0x606aa4d2, {0x93, 0xdd, 0x91, 0xeb, 0x00, 0x00, 0x00, 0x08}},   /* fflush */
    {0x606aa784, {0x93, 0xdd, 0xdf, 0xe8, 0x00, 0x00, 0x00, 0x08}},   /* remove */
    {0x606aa81f, {0x93, 0x82, 0xdd, 0x43, 0xe8, 0x00, 0x00, 0x00}},   /* mkdir */
    {0x606ab659, {0x3d, 0x51, 0xcd, 0xa4, 0xf5, 0xf0, 0x24, 0xa0}},   /* exists */
    {0x606aa646, {0x87, 0x82, 0x93, 0xdd, 0x1b, 0xea, 0x00, 0x00}},   /* fseek */
    {0x606aa5dc, {0x93, 0xdd, 0x87, 0xea, 0x00, 0x00, 0x00, 0x08}},   /* ftell */
    {0x60111291, {0xdd, 0xe7, 0x12, 0xf6, 0xff, 0xa0, 0x14, 0xdf}},   /* the clock's date and time */
    {0x602dd18d, {0xf8, 0xfe, 0xfc, 0x80, 0x6f, 0xdd, 0x39, 0x3f}},   /* callback of setting 111 */
    {0x608442e6, {0xf1, 0xe7, 0xf1, 0xe2, 0x3c, 0x20, 0x08, 0x43}},   /* data cache range invalidation */
    {0x60051ace, {0x93, 0xf1, 0xe6, 0x82, 0xfc, 0xac, 0xe4, 0x1f}},   /* acre_Event1 */
    {0x60051143, {0x5b, 0x38, 0x93, 0x96, 0x86, 0x42, 0x1c, 0x00}},   /* OlyEvWt */
    {0x60051078, {0x86, 0x83, 0x9a, 0x00, 0xcd, 0xba, 0x28, 0xc0}},   /* OlyEvSt */
    {0x6005123a, {0x86, 0x83, 0x9a, 0x00, 0xcd, 0xf8, 0x26, 0xc0}},   /* OlyEvCl */
    {0x60023796, {0xfa, 0xc8, 0x80, 0x00, 0xc5, 0x05, 0x00, 0xca}},   /* scheap_alloc */
    {0x6089e157, {0xfc, 0xa0, 0x18, 0x41, 0xe4, 0x61, 0xb0, 0x00}},   /* CameraCommon */
    {0x608591a7, {0x92, 0xfc, 0xce, 0xf0, 0xad, 0xb4, 0x60, 0x6a}},   /* HFW job constructor */
    {0x60858d8e, {0xfc, 0xa0, 0x6c, 0x20, 0xe4, 0x61, 0xb0, 0x00}},   /* HFW callback executor */
    {0x60858e51, {0x82, 0x43, 0x04, 0x97, 0xbf, 0x00, 0xc9, 0x05}},   /* HFW job post */
    {0x60058ae0, {0xfc, 0x81, 0xb0, 0x00, 0xf0, 0x8f, 0x80, 0x01}},   /* reset on request */
};

const char *drv_name(void)
{
    return "om5m2 fw1100";
}

const char *drv_root(void)
{
    return "C:\\OMKIT\\";
}

char drv_sep(void)
{
    return 0x5c;                            /* backslash */
}

/* The arena is uncached (0xa...), code executes through the cached alias (0x6...), like core itself. A module is
 * linked for this address, so its own variables (data, bss) are used at the cached alias too, through the data cache
 * of the CPU that runs it (CPU 0); core gives the image whole cache lines for that (MEM_LINES). Memory protection
 * watches only 0x60000200..0x60cbe7ff; the arena is above that */
void *drv_exec_addr(void *p)
{
    u32 a = (u32)p;

    return (void *)(a >= 0xa0000000 && a < 0xc0000000 ? a - UNCACHED : a);
}

/* There is no instruction cache invalidation in the firmware at run time, and it is
 * not needed as long as nothing was read or executed at the cached address of the image: core never hands out again the
 * memory of an image that has already run. Data cache: drv_dcache_inv */
void drv_code_sync(void *exec, u32 size)
{
    (void)exec;
    (void)size;
}

int drv_check(void)
{
    u32 i, k;

    for (i = 0; i < sizeof(fingerprint) / sizeof(fingerprint[0]); i++)
        for (k = 0; k < sizeof(fingerprint[0].bytes); k++)
            if (((const volatile u8 *)fingerprint[i].addr)[k] != fingerprint[i].bytes[k])
                return 1;
    return 0;
}

/* One region: the arena in the resource gap, verified with a sample on the camera. The second gap 0xa3cb88f8..0xa3f25540 after verification.
 * Start: past the end of the core image with its stack (__image_end, cached address), rounded up to 4 KB: the core image and stack
 * are written through the cache, the arena's blocks through the uncached address, and a shared cache line would corrupt a
 * module (core 0.53 on the camera). Inside the arena only module images are used through the cache; each has its own lines */
extern u32 __image_end[];

int drv_mem_region(u32 i, void **base, u32 *size)
{
    u32 lo = ((u32)(uptr)__image_end + UNCACHED + 0xfff) & ~0xfffu;

    if (i)
        return 0;
    if (lo < ARENA_LO)
        lo = ARENA_LO;
    *base = (void *)lo;
    *size = ARENA_HI - ARENA_TAIL - lo;
    return 1;
}

/* Only the NMI handler writes task, it is the task number. Vector 0x60030020 updates sp, psw, pc of CPU 0 on every interrupt.
 * RAM is lost at power-off: the block is almost all ones with random flipped bits, so the value matters, not the difference from ffffffff */
int drv_crash_read(int cpu, omk_crash *out)
{
    const volatile u32 *src = (const volatile u32 *)(CRASH_BASE + cpu * CRASH_STEP);
    int i;

    out->task = src[0];
    out->sp = src[1];
    out->psw = src[2];
    out->pc = src[3];
    out->cause = src[4];
    for (i = 0; i < 8; i++)
        out->regs[i] = src[8 + i];
    return out->task > 0 && out->task < 0x1000;
}

/* Jump in firmware code through the uncached address: memory protection watches only 0x6....
 * Verified on the camera. The five bytes do not cross an instruction cache line, the first byte is written last */
/* Under the firmware lock: no task of CPU 0 can run the site while bytes 1..4 are new and byte 0 is still the old opcode.
 * Byte 0 goes last, so until then the old instruction's first byte stays. CPUs 1 and 2 are not stopped: sites they run
 * (live view) are a known residual risk of the order */
int drv_code_jump(u32 site, const u8 *before, void *target)
{
    volatile u8 *p = (volatile u8 *)(site + UNCACHED);
    u32 rel = (u32)target - site, st;
    int k;

    if (site / ICACHE_LINE != (site + JMP_LEN - 1) / ICACHE_LINE)
        return 2;
    st = drv_lock();
    for (k = 0; k < JMP_LEN; k++)
        if (p[k] != before[k]) {
            drv_unlock(st);
            return 1;
        }
    for (k = 1; k < JMP_LEN; k++)
        p[k] = rel >> ((k - 1) * 8);
    p[0] = JMP_D32;
    drv_unlock(st);
    return 0;
}

int drv_dcache_inv_fw(u32 addr, u32 len);    /* thunks.s */

/* Only lines that cannot be dirty: the protected range is never written through the cache (that would be an NMI), so
 * dropping its lines loses nothing; elsewhere a dirty line of firmware data would be lost */
int drv_dcache_inv(u32 addr, u32 len)
{
    if (addr < GUARD_LO || addr + len > GUARD_HI || addr + len < addr)
        return -1;
    return drv_dcache_inv_fw(addr, len);
}

/* Uncached firmware heap scheap_phyalloc 0x60023818 (implementation 0x60023b01): one for all CPUs, under a hardware semaphore.
 * Class: first the blocks never handed out yet (ISSUED counter), then the stack of freed blocks (FREED).
 * Tables 0x60028288..0x600282d4: block counts 178, 21, 43, stride including header 192, 2112, 16448. */
#define FWHEAP_ISSUED 0xa26dbc84
#define FWHEAP_FREED 0xa26dbc94
#define FWHEAP_CLASSES 3

int drv_fwheap_class(u32 i, u32 *block, u32 *total, u32 *free)
{
    static const u32 blocks[FWHEAP_CLASSES] = {128, 2048, 16384}, totals[FWHEAP_CLASSES] = {178, 21, 43};

    if (i >= FWHEAP_CLASSES)
        return 0;
    *block = blocks[i];
    *total = totals[i];
    *free = totals[i] - ((const volatile u32 *)FWHEAP_ISSUED)[i] + ((const volatile u32 *)FWHEAP_FREED)[i];
    return 1;
}

int drv_boot_event(u32 i, char *name, u32 *time)
{
    const volatile u8 *e = (const volatile u8 *)(BOOT_EVENTS + i * BOOT_EVENT_SIZE);
    u32 k;

    if (i >= *(const volatile u16 *)BOOT_COUNT)
        return 0;
    for (k = 0; k < BOOT_NAME_LEN; k++)
        name[k] = e[k];
    name[BOOT_NAME_LEN] = 0;
    *time = *(const volatile u32 *)(e + BOOT_NAME_LEN);
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------
 * Events. Hook: a jmp to a trampoline in thunks.s, installed by drv_code_jump. */
typedef struct {
    u32 d0, d1, a0, a1, arg4, result;       /* order as in the thunks.s trampolines */
} hook_regs;

void drv_tramp_pm(void);
void drv_tramp_s1(void);
u32 drv_orig_pm(hook_regs *r);

#define PM_REJECTED 0x100                   /* response of 0x602d78a3: a rule or ONOFF replaced the mode */

static const struct {
    u32 event, site, phases;
    u8 before[JMP_LEN];
    void (*tramp)(void);
} hooks[] = {
    /* availability handler 111: entry, the displaced instructions are movm [d2],(sp); add -4,sp */
    {OMK_EV_PICTMODE_CHECK, 0x602d7964, OMK_PH_NOTIFY_BEFORE | OMK_PH_REPLACE | OMK_PH_FILTER_AFTER | OMK_PH_NOTIFY_AFTER,
     {0xcf, 0x80, 0xf8, 0xfe, 0xfc}, drv_tramp_pm},
    /* API 0x80001: entry, the displaced instruction is mov 0x80001,d0 (6 bytes); site verified on the camera, core 0.11 */
    {OMK_EV_S1_API, 0x60323a64, OMK_PH_NOTIFY_BEFORE, {0xfc, 0xcc, 0x01, 0x00, 0x08}, drv_tramp_s1},
};

#define NHOOKS (sizeof(hooks) / sizeof(hooks[0]))

int drv_ev_supported(u32 event, u32 *phases)
{
    u32 i;

    for (i = 0; i < NHOOKS; i++)
        if (hooks[i].event == event) {
            *phases = hooks[i].phases;
            return 1;
        }
    return 0;
}

int drv_ev_install(u32 event)
{
    u32 i;

    for (i = 0; i < NHOOKS; i++)
        if (hooks[i].event == event)
            return drv_code_jump(hooks[i].site, hooks[i].before, (void *)hooks[i].tramp);
    return 3;
}

static void pm_orig(void *data, void *ctx)
{
    hook_regs *r = ctx;
    omk_ev_pictmode_check *d = data;

    r->result = drv_orig_pm(r);
    d->allowed = (r->result & 0xffff) == 0;
}

/* Calls of the event hooks on CPUs 1 and 2 (the original runs without handlers there). Hooks on those CPUs must not touch
 * cached core variables (the data caches of the CPUs are not coherent): the counters live in their own 64 bytes, used only through the uncached
 * alias, so no CPU ever holds their line in its cache. Increments from two CPUs may race: a statistic, not a decision */
static u32 other_cpu_slot[16] __attribute__((aligned(64), section(".data.cpu_shared")));

static volatile u32 *other_cpu_calls(void)
{
    return (volatile u32 *)((u32)other_cpu_slot + UNCACHED);
}

u32 drv_ev_other_cpu(u32 event)
{
    return event < 16 ? other_cpu_calls()[event] : 0;
}

/* From the trampoline. Response to the firmware: the original one if modules did not change allowed, otherwise 0 or PM_REJECTED */
void drv_hook_pm(hook_regs *r)
{
    omk_ev_pictmode_check d;

    if (drv_cpu() != 0) {
        other_cpu_calls()[OMK_EV_PICTMODE_CHECK]++;
        r->result = drv_orig_pm(r);
        return;
    }
    d.mode = r->a1 & 0xffff;
    d.allowed = 1;
    if (!ev_dispatch(OMK_EV_PICTMODE_CHECK, &d, pm_orig, r) || d.allowed != ((r->result & 0xffff) == 0))
        r->result = d.allowed ? 0 : PM_REJECTED;
}

static void s1_orig(void *data, void *ctx)
{
    (void)data;
    (void)ctx;                              /* the original continues by itself after the trampoline */
}

void drv_hook_s1(void)
{
    if (drv_cpu() != 0) {
        other_cpu_calls()[OMK_EV_S1_API]++;
        return;
    }
    ev_dispatch(OMK_EV_S1_API, 0, s1_orig, 0);
}

/* ------------------------------------------------------------------------------------------------------------------
 * Screen or viewfinder, for the power probe line of the log: video compositor channel 0, scale of OSD plane 2. On the camera
 * the screen has no scaling (0), the viewfinder has it (0x11, window 1024 x 768): .
 * Read only (the drawing into the OSD buffers was removed in core 0.94) */
#define VSC0 0x9c000000
#define VSC_RSZ_OSD3 0x178

static u32 vsc(u32 off)
{
    return ((const volatile u32 *)VSC0)[off / 4];
}

/* ------------------------------------------------------------------------------------------------------------------
 * Power hooks. The sites' bytes are checked against the
 * image; they run in the application task - or, for the restart past the application (drv_restart_hfw), in the HFW
 * callback task HLS_CALLBACK; both are tasks of CPU 0. The kinds in thunks.s are POWER_HOOK_* of omk_power.h */
void drv_tramp_off(void);
void drv_tramp_card(void);
void drv_tramp_sleep(void);
void drv_tramp_gate(void);
void drv_tramp_wake(void);

static const struct {
    const char *name;
    u32 site;
    u8 before[JMP_LEN];
    void (*tramp)(void);
} power_hooks[] = {
    {"power-off", 0x6089e774, {0xcf, 0xf0, 0xf8, 0xfe, 0xe4}, drv_tramp_off},
    {"card off", 0x601b3267, {0xfc, 0xdc, 0xc9, 0x0a, 0x21}, drv_tramp_card},
    {"sleep", 0x6089e863, {0xcf, 0xa0, 0xf8, 0xfe, 0xf8}, drv_tramp_sleep},
    {"sleep gate", 0x60325f80, {0x82, 0xfc, 0xcc, 0x01, 0x00}, drv_tramp_gate},
    {"wake", 0x60898419, {0x00, 0x42, 0x14, 0x8f, 0x23}, drv_tramp_wake},
};

int drv_power_install(u32 i, const char **name)
{
    if (i >= sizeof(power_hooks) / sizeof(power_hooks[0]))
        return -1;
    *name = power_hooks[i].name;
    return drv_code_jump(power_hooks[i].site, power_hooks[i].before, (void *)power_hooks[i].tramp);
}

/* From the trampolines. Only CPU 0: the core state lives in cached memory of CPU 0 */
void drv_hook_power(u32 kind, u32 arg)
{
    if (drv_cpu() == 0)
        power_hook(kind, arg);
}

/* CameraCommon::shutDown d1 */
const char *drv_power_reason(u32 reason)
{
    switch (reason) {
    case 0:
        return "card unmount without the power-off hook";
    case 1:
        return "power lever";
    case 4:
        return "deep sleep (auto power off)";
    case 5:
        return "battery";
    case 10:
        return "quick sleep";
    case 11:
        return "reboot (a restart request)";
    }
    return 0;
}

/* "Power lever off accepted" (0x61e41990), K1: set to 1 by the system event
 * forwarding (event 39) and by the lever check on mode changes, before CameraCommon::shutDown; cleared only when
 * startShutDown gives up (cancel). Lever only. Read through the cached address on CPU 0, as the firmware does */
#define LEVER_OFF_FLAG 0x61e41990

int drv_power_pending(void)
{
    return *(const volatile u32 *)LEVER_OFF_FLAG == 1;
}

static int ram_ptr(u32 p)
{
    return p >= 0x60000000 && p < 0x64000000 && !(p & 3);
}

/* For the log: K1 lever flag, K3 (0x61e42010) OlyHfwPower began power-off or sleep, K5 UI state word (0x6234f838, read at
 * its uncached alias 0xa234f838 - see GUI_STATE in pictprof.c; bytes: group, state; 0x0104 - black screen), K4 raw input byte group 0 (0xa26dea7c, uncached; bit 0 - lever in the emulator),
 * (0x62151ee8), K2 application state (+192 of the object reached from 0x61e497d8 -> +0xd8 -> +0x70 = object + 20;
 * a hypothesis from the emulator, every pointer checked; 0xffffffff - path broken). */
u32 drv_power_probe(u32 *w, u32 max)
{
    u32 v[7], n, p;

    v[0] = *(const volatile u32 *)LEVER_OFF_FLAG;
    v[1] = *(const volatile u32 *)0x61e42010;
    v[2] = *(const volatile u32 *)0xa234f838;   /* past the cache, as every reader of the GUI state (pictprof.c) */
    v[3] = *(const volatile u8 *)0xa26dea7c;
    v[4] = *(const volatile u32 *)0x62151ee8;
    v[5] = 0xffffffff;
    p = *(const volatile u32 *)0x61e497d8;
    if (ram_ptr(p)) {
        p = *(const volatile u32 *)(p + 0xd8);
        if (ram_ptr(p)) {
            p = *(const volatile u32 *)(p + 0x70);
            if (ram_ptr(p))
                v[5] = *(const volatile u32 *)(p + 172);
        }
    }
    /* 1 - the viewfinder: its plane is scaled, the screen's is not. The only register here, read only when asked for (the
     * log's probe line); the passing-state checks of every pass (drv_ui_probe) read RAM only */
    v[6] = max > 6 ? vsc(VSC_RSZ_OSD3) != 0 : 0;
    for (n = 0; n < max && n < 7; n++)
        w[n] = v[n];
    return n;
}

/* Application states (K2) that last a moment in normal work: 0x5101 half-press, 0x0002 a transition; the UI hangs
 * after a shot (core 0.90-0.93, the on-screen text) stayed in them. The input byte K4 changes with the keys of group 0
 * (bit 1 cleared on half-press: a hypothesis) */
#define APP_HALF_PRESS 0x5101
#define APP_TRANSITION 0x0002

int drv_ui_probe(u32 *app, u32 *keys)
{
    u32 w[6];

    if (drv_power_probe(w, 6) < 6 || w[5] == 0xffffffff)
        return -1;
    *app = w[5];
    *keys = w[3];
    return w[5] == APP_HALF_PRESS ? DRV_UI_HALF : w[5] == APP_TRANSITION ? DRV_UI_TRANSIENT : 0;
}

/* The test scenario's shutter button (core/shoot.c). Keys: API 0xD0002 (0x6032b57f -> 0x601310b8), the path of the
 * shell's "ui key skey": a snapshot of the key image with the key's bit goes into the key task's ring (10 entries) and the
 * key task (UI_IN, priority 44, above core) is woken, so everything after it - a lone full press preceded by the
 * half-press, the application's queue - is the button's. State 0 is pressed, 1 released (the firmware's own callers and
 * the key decoder 0x60131840; static reading of E148 1.1). The keys' events: 0x2000100 Trg1 (half), 0x2000101 Trg2.
 * Wifi: the events the Wi-Fi remote's exec_shutter.cgi posts (0x61430767): 0x041B0024 / 25 half-press down / up, 26 / 27
 * the full press; past the key task. Core task only, never under drv_lock: the key task runs at once on CPU 0 */
int drv_key_api(u32 event, u32 state);     /* thunks.s */
void drv_app_event(u32 event, u32 arg);

/* The reboot request: 0x041B006B into the application's queue (0x60046624), as the firmware's PTP operation 0x970A
 * (0x614bed6b -> 0x614beec2) and the Wi-Fi page exec_reboot.cgi (0x6142e328) post it. The event becomes the HFW event
 * 0x14f (table 0x60bc774c) and the application's OLY_WIFI_REQ_BOOT 0x01060011 (table 0x60fb1e8c); the PC sub-application's
 * common table (0x60f8951c) runs 0x60d1629b for it in any of its states: CameraCommon::shutDown(1, 11, 0) in the
 * application's task - startShutDown's reason 11 is mode 0 with a wake-up alarm of 1000 ms (table 0x60b7e16c). Shooting,
 * playback and the menus have no entry for it (static reading of E148 1.1) */
#define EV_REBOOT_REQUEST 0x041B006B

int drv_restart_request(void)
{
    drv_app_event(EV_REBOOT_REQUEST, 0);
    return 0;
}

/* The restart past the application (the hands-free scenario's end: the camera shoots, where the request above is
 * ignored). A job for the HFW callback executor (OlyHfwCallbackManager 0x62186cf0, task HLS_CALLBACK, CPU 0, priority 54,
 * stack 14848) whose callback is CameraCommon::shutDown(1, 11, 0) itself - the way the factory reset posts its power-off
 * job (0x6089ab75 -> startShutDown(1, 9, 0) in HLS_CALLBACK). The power-off is then the reboot of the PC mode's request:
 * the settings saved, the card unmounted (message 53), an alarm of 1 s; the power hooks run in HLS_CALLBACK, so the core
 * task stays free to acknowledge the card-off gate. Executor +820, u16: jobs refused - the HFW event channel sets it while
 * it waits (0x60858f6d), and a job posted then is deleted unrun (0x60858e5c). The job's 28 bytes come from scheap
 * (0x60023796), which its deletion frees (0x6085947b -> 0x600237c8). Static reading of E148 1.1. Core task (CPU 0, as
 * HLS_CALLBACK and the heap), never under drv_lock: HLS_CALLBACK is to run at once */
#define HFW_JOBS_REFUSED 820
#define HFW_JOB_SIZE 28

u32 drv_scheap_alloc(u32 size);             /* thunks.s */
u32 drv_camera_common(void);
u32 drv_hfw_job_off(u32 mem, u32 camera_common);
u32 drv_hfw_executor(void);
void drv_hfw_post(u32 executor, u32 job);
void drv_fw_reset(u32 cause);

int drv_restart_hfw(void)
{
    u32 exec = drv_hfw_executor(), cc, mem;

    if (!ram_ptr(exec))
        return -1;
    if (*(const volatile u16 *)(exec + HFW_JOBS_REFUSED))
        return -2;
    cc = drv_camera_common();
    if (!ram_ptr(cc))
        return -1;
    mem = drv_scheap_alloc(HFW_JOB_SIZE);
    if (!mem)
        return -3;
    drv_hfw_post(exec, drv_hfw_job_off(mem, cc));
    return 0;
}

/* The last resort: the firmware's reset on request 0x60058ae0 - the cause into (0x8ff000b0), which no start clears, and the inter-CPU interrupt of CPU 0, whose handler stops feeding the watchdog: a reset
 * about 1 s later, a full start with the loader (camera: the reboot loops of core 0.53 and 0.62-0.63 end the same way).
 * Nothing is saved, the card is not unmounted. Does not return */
void drv_reset(u32 cause)
{
    drv_fw_reset(cause);
    for (;;)
        drv_sleep(1000);
}

/* ------------------------------------------------------------------------------------------------------------------
 * The USB hold (core/handsfree.c). The application (its task, CPU 0) applies the USB choice in checkPcMode 0x60d1552f:
 * the getter 0x60d079e2 reads (0x621bcb1c), the value picks the PC sub-application's next event (table 0x60f7ad80):
 * 0 the dialog (USB_SELECT), 1 and 2 START_PC (Storage, MTP), 3 START_USB_PD - "charge only" 0x60d163fc:
 * setUsbControlType(0, 1), then 0x60d1647c reads the value again and at 3 goes to the shooting application. Every way
 * into the PC mode goes through it: the start with the cable in (MOUNT_END), the USB setting read (0x09000014), the end
 * of the dialog, a display change. The hook sits right after the read (0x60d15549, displaced mov d0,d2; mov sp,a0;
 * add 8,a0; mov d3,(a0)): with the hold on, d0 and the word become the mode held. The word is written through the cache:
 * the hook runs in the application's task on CPU 0, whose cache its own setter 0x60d079d9 and getter use; it lies
 * outside the range the memory guard watches. The hold and the counters are read by the hook and written by the core
 * task: a line of their own, used only through the uncached alias. Static reading of E148 1.1 */
#define USB_SITE 0x60d15549
#define USB_CHOICE 0x621bcb1c
#define USB_FACTOR 0x621bcbcc               /* the start's factor (getter 0x60d12988): 4 - the cable to a host at the mount */
#define USB_TPC_STATE 0xa23a0b04            /* u8, TPC_CAMC's state (0x614ba240 writes 2): 1 in MTP, 2 in the dialog and in charge
                                               only (camera, card163); read past the cache, for the log */
#define KEY_OK 0x02000004                   /* the OK key's event: code 4 "Ok" (names 0x60258ec8), as Trg1 is 0x2000100 */

enum { USB_MODE, USB_HITS, USB_LAST, USB_SLOTS = 16 };
static u32 usb_slot[USB_SLOTS] __attribute__((aligned(64), section(".data.cpu_shared")));
static int usb_installed;

void drv_tramp_usb(void);

static volatile u32 *usb(void)
{
    return (volatile u32 *)((u32)usb_slot + UNCACHED);
}

void drv_usb_filter(u32 *d0)
{
    volatile u32 *u = usb();
    u32 mode = u[USB_MODE];

    if (!mode || drv_cpu() != 0)
        return;
    u[USB_LAST] = *d0;
    u[USB_HITS]++;
    *d0 = mode;
    *(volatile u32 *)USB_CHOICE = mode;
}

int drv_usb_hold_install(void)
{
    static const u8 before[JMP_LEN] = {0x82, 0x3c, 0x20, 0x08, 0x6c};
    int rc;

    if (usb_installed)
        return 0;
    rc = drv_code_jump(USB_SITE, before, (void *)drv_tramp_usb);
    usb_installed = !rc;
    return rc;
}

void drv_usb_hold(u32 mode)
{
    usb()[USB_MODE] = mode;
}

u32 drv_usb_hold_get(void)
{
    return usb()[USB_MODE];
}

u32 drv_usb_hold_hits(void)
{
    return usb()[USB_HITS];
}

void drv_usb_probe(drv_usb_info *u)
{
    u32 w[6];

    u->choice = *(const volatile u32 *)USB_CHOICE;   /* the core task is on CPU 0, as the application: the same cache */
    u->factor = *(const volatile u32 *)USB_FACTOR;
    u->link = *(const volatile u8 *)USB_TPC_STATE;
    u->app = drv_power_probe(w, 6) == 6 ? w[5] : 0xffffffff;
}

int drv_ok_key(int down)
{
    return drv_key_api(KEY_OK, down ? 0 : 1) & 0xffff;
}

int drv_shutter(u32 key, int down, u32 path)
{
    if (key > DRV_SHUTTER_FULL)
        return -1;
    if (path == DRV_SHUTTER_WIFI) {
        drv_app_event(0x041B0024 + 2 * key + (down ? 0 : 1), 0);
        return 0;
    }
    if (path != DRV_SHUTTER_KEYS)
        return -1;
    return drv_key_api(0x2000100 + key, down ? 0 : 1) & 0xffff;
}

/* Media busy flags: the camera's power code waits while (0x61e418f0) = 1 or (0x61e418f4) != 0
 * before sleep. A hypothesis that this is the camera's own card work (a picture being written); core's writes do not set
 * them. Read through the cache on CPU 0, like the firmware */
/* The firmware's open file (the pointer fopen gives, F = slot + 16 in a pool of 6 slots, (0x62151c98)): +0 u16 1 - in use
 * (0x606ba8d1), +296 u16 the file's number from the counter (0x62151ec0), new at every fopen (0x606b9b16, 0x606ba8ef). A
 * mount or an unmount of the card frees every file of the drive without writing anything (0x606b99c4: unmount 0x60698259,
 * mount 0x6068d81f) and its own check (0x606b9a5e) then refuses the pointer with 0x9414 - until the slot is given to the
 * next fopen, which takes the freed slot first (LIFO, 0x606b9821). The number tells the two apart. Static reading of
 * E148 1.1; the refusal - camera, card157 (the PC mode over USB). Read by the core task on CPU 0, where the file task runs */
u32 drv_file_id(void *fp)
{
    const volatile u16 *f = (const volatile u16 *)fp;

    if (!fp || f[0] != 1)
        return 0;
    return 0x10000u | f[296 / 2];
}

/* The volume of drive 0 (C:) mounted: the u16 the file system's mount sets to 1 (0x606983ab) and its unmount to 0
 * (0x6069829d), the bit 0x10000 of the media state request 0x2000f (0x606961ff); read past the cache, as it is kept */
int drv_card_mounted(void)
{
    return *(const volatile u16 *)0xa26423c8 == 1;
}

int drv_card_busy(void)
{
    return *(const volatile u32 *)0x61e418f0 == 1 || *(const volatile u32 *)0x61e418f4 != 0;
}

/* ------------------------------------------------------------------------------------------------------------------
 * The core task's wait: an RTOS event flag instead of a plain sleep, so a hook can wake it at once (power-off, sleep, wake)
 * rather than after up to 250 ms. The firmware's own tasks wait this way (OlyEvWt with a timeout). Services work on the
 * objects of their own CPU only: the flag belongs to CPU 0, it is set from tasks of CPU 0 only, never under drv_lock.
 * If the flag cannot be made or the wait misbehaves, plain dly_tsk is used. Misbehaving is: the wait fails at once
 * (an error code within WAIT_QUICK_US) WAIT_FAIL_MAX times in a row, or it "succeeds" at once more than WAIT_SPIN_MAX
 * times within WAIT_SPIN_US - a flag that never clears. A wake-up that was already there when the wait began is a
 * success at once and legitimate (dial steps in an editor come faster than the loop serves them): it never counts as a
 * failure, and hooks cannot produce hundreds of them a second */
int drv_ev_create(const char *name);
int drv_ev_wait(int id, u32 ms);
int drv_ev_set(int id);
int drv_ev_clear(int id);

#define WAIT_QUICK_US 2000                  /* a return sooner than this was not a wait */
#define WAIT_FAIL_MAX 50                    /* failures at once in a row */
#define WAIT_SPIN_US 1000000                /* the window of the count of successes at once ... */
#define WAIT_SPIN_MAX 500                   /* ... and its limit: a stuck flag gives thousands a second, hooks a few dozen */

static int core_flag;                       /* > 0 - the flag id; 0 - none; < 0 - given up */
static u32 quick_fails, quick_wakes, wakes_t0;

int drv_wait_init(int on)
{
    if (!on || core_flag)
        return core_flag;
    core_flag = drv_ev_create("OMK_CORE");
    if (core_flag <= 0)
        core_flag = -1;
    return core_flag;
}

/* Core task only. 1 - woken by a hook, 0 - the time ran out (or the plain sleep) */
int drv_wait(u32 ms)
{
    u32 t0;
    int rc;

    if (core_flag <= 0) {
        drv_sleep(ms);
        return 0;
    }
    t0 = drv_ticks();
    rc = drv_ev_wait(core_flag, ms);
    drv_ev_clear(core_flag);                /* whatever the flag's attribute: the next wait waits again */
    if (drv_ticks_since(t0) >= WAIT_QUICK_US || ms < 10) {
        quick_fails = 0;                    /* a real wait (or one too short to tell) */
    } else if (rc) {
        if (++quick_fails >= WAIT_FAIL_MAX) {   /* the wait refuses: never spin, fall back */
            core_flag = -1;
            return 0;
        }
    } else {                                /* woken at once: legitimate unless it never stops */
        quick_fails = 0;
        if (!quick_wakes || drv_ticks_since(wakes_t0) >= WAIT_SPIN_US) {
            quick_wakes = 0;
            wakes_t0 = t0;
        }
        if (++quick_wakes > WAIT_SPIN_MAX) {
            core_flag = -1;
            return 0;
        }
    }
    return rc == 0;
}

/* From a task on CPU 0, outside drv_lock: the core task stops waiting. Elsewhere: nothing, it notices by its timeout */
void drv_wake(void)
{
    if (core_flag > 0 && drv_cpu() == 0)
        drv_ev_set(core_flag);
}

int drv_wait_state(void)
{
    return core_flag;
}

/* ------------------------------------------------------------------------------------------------------------------
 * Subprocessor clock: seconds at 0x98e00098, 1/1024 s in bits 5..14 of
 * 0x98e0009c; it keeps counting while the camera sleeps. The firmware checks the block id first: ((0x98e00000) >> 2) & 63 = 42.
 * The two words are read apart: the seconds are read again to catch a carry in between */
int drv_rtc(u32 *sec, u32 *frac1024)
{
    volatile const u32 *r = (volatile const u32 *)0x98e00000;
    u32 s1, f, s2, k;

    if (((r[0] >> 2) & 63) != 42)
        return -1;
    for (k = 0; k < 3; k++) {
        s1 = r[0x98 / 4];
        f = r[0x9c / 4];
        s2 = r[0x98 / 4];
        if (s1 == s2) {
            *sec = s1;
            *frac1024 = (f >> 5) & 0x3ff;
            return 0;
        }
    }
    return -1;
}

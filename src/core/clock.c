#include "omk_core.h"

#define TICKS_MASK DRV_TICKS_MASK

static u32 last_raw, us_rest, ms_total;
static int started;

u32 omk_ms(void)
{
    u32 st = drv_lock(), raw = drv_ticks(), ms;

    if (!started) {
        started = 1;
        ms_total = raw / 1000;
        us_rest = raw % 1000;
    } else {
        us_rest += (raw - last_raw) & TICKS_MASK;
        ms_total += us_rest / 1000;
        us_rest %= 1000;
    }
    last_raw = raw;
    ms = ms_total;
    drv_unlock(st);
    return ms;
}

#ifdef OMK_DEBUG
/* The tick counter wraps every ~1073 s: a sleep longer than that loses whole turns. The caller measured the sleep by a clock
 * that keeps counting (drv_rtc) and adds what the ticks could not see */
void omk_ms_add(u32 ms)
{
    u32 st;

    omk_ms();                               /* first take what the ticks did see */
    st = drv_lock();
    ms_total += ms;
    drv_unlock(st);
}
#endif

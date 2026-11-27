/* Log for core and modules: a RAM buffer, written to the card by the core service loop, not by the calling task. */
#ifndef OMK_LOG_H
#define OMK_LOG_H

#include <stdarg.h>
#include "omk_api.h"                /* OMK_LOG_* levels */
#include "omk_drv.h"

/* A release build (omk_types.h) keeps the short log only: levels up to info. The lines of the debug level, the boot
 * checkpoints and the debug service's tee are compiled into a debug build alone (LOG_MAX) */
#ifdef OMK_DEBUG
#define LOG_MAX OMK_LOG_DEBUG
#else
#define LOG_MAX OMK_LOG_INFO
#endif

void log_setup(int enabled, int level);     /* a level above LOG_MAX is taken as LOG_MAX */
int log_level(void);                    /* the most detail anyone keeps: the card's level or the tee's */
#ifdef OMK_DEBUG
/* The debug service (core 1.40): fn gets every line up to lvl with its time, without its line end, under the firmware lock
 * (a copy into RAM only: it must not take the lock again); log_card(0) - nothing is written to the card from now on, nor kept for it */
typedef void (*log_tee_fn)(int lvl, u32 ms, const char *text, u32 len);
void log_tee(log_tee_fn fn, int lvl);
void log_card(int on);
#endif

/* Format: %s %c %d %u %x %X, zero-padded width for numbers: %08x. The line is terminated automatically */
void log_printf(int level, const char *fmt, ...);
void log_vprintf(int level, const char *fmt, va_list ap);
void log_vprintf_from(int level, const char *who, const char *fmt, va_list ap);   /* line "who: text" */

void log_flush(void);                   /* service loop only: file calls */
/* The log file is kept open between writes (log.c): closed by the core task when the camera leaves the running state,
 * before a file job of the debug service, and by the card-off gate after its last flush */
void log_close(void);
#ifdef OMK_DEBUG
u32 log_room(void);                     /* free bytes in the ring */
/* Boot checkpoint, core task only: at debug level the line goes to the card at once, so a crash right after it leaves a
 * trace (the camera may go down before the next regular write). Below debug - a debug line that is not even kept */
void log_checkpoint(const char *fmt, ...);
#else
/* A release build: no checkpoints, and a call of log_printf with the constant level OMK_LOG_DEBUG is not compiled at all -
 * neither the call nor its text, and its arguments are not evaluated. (The function's own definition and a call through
 * (log_printf) are not touched by the macro.) A level known only at run time is dropped by the level check, as always */
#define log_checkpoint(...) ((void)0)
#define log_printf(lvl, ...) ((lvl) == OMK_LOG_DEBUG ? (void)0 : log_printf((lvl), __VA_ARGS__))
#endif
/* A diagnostic line a debug build writes at the info level - the details of every picture, the hooks' counters. A release
 * build's short log has none of them: not compiled there */
#ifdef OMK_DEBUG
#define log_diag(...) log_printf(OMK_LOG_INFO, __VA_ARGS__)
#else
#define log_diag(...) (0 ? log_printf(OMK_LOG_INFO, __VA_ARGS__) : (void)0)   /* dead: neither the call nor its text */
#endif
void log_service(u32 now_ms);
void log_retry_now(void);               /* the card may be back: no pause before the next try (see log.c) */
/* The header of a run: an empty line, then the line at info level (CORE.LOG holds many runs one after another) */
void log_banner(const char *fmt, ...);

#endif

/* core services shared by its parts: time, memory, config. */
#ifndef OMK_CORE_H
#define OMK_CORE_H

#include "omk_drv.h"
#include "omk_mem.h"

#define CORE_VERSION "1.45"
#ifndef CORE_BUILD                          /* the Makefile: git branch@commit of the build, "-dirty" with changes */
#define CORE_BUILD "unknown"
#endif

/* Milliseconds since camera start. The driver counter wraps every 1073 s, so call this at least once every 17 minutes */
u32 omk_ms(void);
void omk_ms_add(u32 ms);                    /* clock.c: time the tick counter could not see (a long sleep) */

/* Config C:\OMKIT\OMKIT.INI: sections, key = value, comments ; and #. Names are case-insensitive */
int cfg_load(void);                     /* 1 - read, 0 - did not exist, sample created, -1 - failed */
const char *cfg_str(const char *section, const char *key, const char *def);
int cfg_int(const char *section, const char *key, int def);
int cfg_bool(const char *section, const char *key, int def);
int cfg_size(const char *section, const char *key, u32 *out);   /* bytes, suffixes K and M: 1 - present, 0 - no such key, -1 - not a number */
u32 cfg_count(void);
void cfg_entry(u32 i, const char **section, const char **key, const char **value);
u32 cfg_bad_lines(u32 *first);          /* how many lines failed to parse, and the number of the first one */
/* A one-shot file of the OMKit folder with keys of one section (no header, or that section's), read after OMKIT.INI:
 * its keys win. 1 read (wrong: its lines not understood), 0 not there, -1 longer than 1 KB (not used). Once per start */
int cfg_once(const char *name, const char *section, u32 *wrong);
void cfg_once_drop(void);                   /* its keys forgotten (it could not be consumed) */
const char *cfg_once_str(const char *section, const char *key);   /* a key of the one-shot file itself, or 0 */

int str_ieq(const char *a, const char *b);
u32 str_len(const char *s);

#define OMK_PATH_MAX 64
int omk_path(char *out, const char *dir, const char *name);   /* OMKit root + [dir\] + name; -1 - too long */
/* The OMKit folder keeps only OMKIT.INI, CORE.BIN and MODULES; the rest goes into folders */
#define LOG_DIR "LOG"                       /* CORE.LOG, the log of the runs one after another */
#define STORE_DIR "STORE"                   /* the stores of core and the modules */
int omk_dir(const char *dir);               /* OMKit root + dir made if missing: 0, or -1 */
void api_res_batch(int on);                 /* api.c: 1 - module_init runs, its resource reads share one open file */
int omk_name83(char *out, const char *in, int ext);           /* 8.3 name in upper case; -1 - invalid */
int omk_name_ext(char *out, const char *base, const char *ext);   /* base.ext into out[13]; -1 - not a valid 8.3 name */
int omk_name_long(char *out, const char *in);   /* a long card name as is into out[OMK_NAME_MAX]; -1 - invalid */
u32 crc32_update(u32 crc, const void *data, u32 n);           /* start with 0 */


int core_in_task(void);                     /* main.c: 1 - the caller is the core task (by its stack) */
void core_mark_ended(void);                 /* main.c: the run reached the end of a power-off */
int core_prev_ended(void);                  /* main.c: 1 - the previous run ended with a power-off (or it is not known) */
const char *core_run(void);                 /* main.c: this power-on's name (8 hex digits) */
/* debug.c: core is about to write to the card (begin 1) or is done (0) - the log, a store, a file of the debug service;
 * what - the path. Snapshots of the file tasks around it when the debug service is on; nothing otherwise */
void core_card_write(int begin, const char *what);

/* grain.c: the look's grain - the tile containers, the slot and the driver's hooks; its work from the core loop */
void grain_service(u32 now);
struct omk_mod;
u32 grain_list(struct omk_mod *m, char (*files)[13], u32 max);   /* API 17: the grain containers; the first call takes the service */
void grain_drop_module(struct omk_mod *m);  /* the owner failed module_init: no grain, the service is free */
int grain_look(u32 entry, s32 tile, u32 level);   /* the look owner's choice (look.c checks the owner): 0, -1 bad, -2 no service */

#endif

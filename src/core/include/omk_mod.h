/* Modules: the .OMM file format and the core loader.
 * File: 64-byte header | image (code, constants, data; linked at address 0) | relocations u32[relocs] |
 * resource directory omm_res[res_count] | in-memory resource data res_bytes | on-demand data res_disk.
 * CRC32 of the whole file with zero in place of the crc field.
 * Relocation: add the image execution address to the word at the offset (may be unaligned: an immediate in an instruction). */
#ifndef OMK_MOD_H
#define OMK_MOD_H

#include "omk_api.h"
#include "omk_core.h"

#define OMM_MAGIC 0x4d4b4d4f                /* 'OMKM' */
#define OMM_FORMAT 1
#define OMM_HEAD 64
#define OMM_NAME 8
#define OMM_RES_ONDEMAND 1                  /* resource read from the card on demand: the offset is within the res_disk data */
#define OMK_RES_STRINGS 0x8000             /* string table, id 0: {u16 count, u16 0, u16 offsets[count]}, zero-terminated strings */

typedef struct {
    u32 magic;
    u16 format, api;                        /* api: API version the module was built with; must not be newer than core */
    char name[OMM_NAME];                    /* upper case, zero-padded; matches the file name */
    u32 version;                            /* upper 16 bits: major, lower: minor */
    u32 image, bss, entry;                  /* image size, size of bss after it, module_init offset */
    u32 relocs, res_count, res_bytes;
    u32 flags, crc;
    u32 res_disk;                           /* bytes of on-demand data at the end of the file */
    u32 reserved[2];
} omm_head;

typedef struct {
    u16 type, id, reserved, flags;          /* no languages: strings are English only */
    u32 offset, size;                       /* within the in-memory data or, with OMM_RES_ONDEMAND, within the on-demand data */
} omm_res;

#define MOD_MAX 16

enum { MOD_LOADING = 1, MOD_RUNNING, MOD_FAILED };

struct omk_mod {
    char name[OMM_NAME + 1];
    int owner, state;
    u32 version, image_size, bss, entry;
    u8 *image;                              /* write address */
    u8 *exec;                               /* execution address */
    omm_res *dir;
    u8 *res;
    u32 res_count, res_bytes, res_disk, disk_at;   /* disk_at: where the on-demand data is in the file */
    u8 *disk;                               /* the on-demand data in memory while module_init runs (module.c), else 0 */
    const u8 *strings;                      /* string table or 0 */
    u32 nstrings;
    char path[OMK_PATH_MAX];                /* module file: for reading resources on demand */
    omk_store *store;
    omk_power_fn power_fn;                  /* api->on_power */
    void *power_ctx;
    void (*tick_fn)(void *ctx);             /* api->every */
    void *tick_ctx;
    u32 tick_ms, tick_at;
    int dir_made;                           /* its folder OMKIT/NAME is there (api.c, the first file call) */
};

int modules_load(void);                     /* from [modules] in OMKIT.INI, in the core task; 0 - stopped by a power-off or
                                               a sleep: call again when the camera runs */
u32 modules_count(void);
omk_mod *module_at(u32 i);

/* Calls module_init; host tests replace it, module code cannot run there */
extern int (*module_call)(void *entry, const omk_api *api, omk_mod *m);

const omk_api *omk_api_get(void);

/* power.c: tell the running modules about a power state (OMK_POWER_*), in the order of api->on_power */
void modules_power(u32 event);

#endif

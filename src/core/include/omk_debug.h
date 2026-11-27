/* The debug service (core/debug.c, core 1.40): a log and state snapshots in RAM that outlives a reset, read over USB
 * with vendor operations of PTP; optionally no card writes by core, and files of the OMKit folder written over USB.
 * Off unless [debug] enabled = on. A debug build only: a release build has no debug service at all, and core_card_ok
 * is always 1 there. */
#ifndef OMK_DEBUG_H
#define OMK_DEBUG_H

#include "omk_types.h"

/* The protocol (the computer's side: tools/omkdebug.py) */
#define DBG_OP_CMD 0x9e00                   /* no data */
#define DBG_OP_GET 0x9e01                   /* data to the computer */
#define DBG_OP_PUT 0x9e02                   /* data from the computer */
#define DBG_PROTOCOL 1
#define DBG_MAGIC 0x444b4d4fu               /* 'OMKD': HELLO's p1 */
#define DBG_RESTART_KEY 0x21544252u         /* 'RBT!': RESTART's p2 */

enum {                                      /* DBG_OP_CMD: p1 */
    DBG_HELLO = 1, DBG_STATUS = 2, DBG_LEVEL = 3, DBG_MARK = 4, DBG_SNAP = 5, DBG_READ = 0x10, DBG_LIST = 0x11,
    DBG_WBEGIN = 0x20, DBG_WCOMMIT = 0x22, DBG_WABORT = 0x23, DBG_RESTART = 0x30, DBG_SHOOT = 0x31,
    DBG_CONTROL_FIRST = 0x40, DBG_CONTROL_LAST = 0x4f   /* camera control: kept free, not done */
};
enum { DBG_SHOOT_STATUS, DBG_SHOOT_START, DBG_SHOOT_STOP };   /* DBG_SHOOT: p2 */
enum { DBG_BANKS = 1, DBG_BANK = 2, DBG_MEM = 3, DBG_HIST = 4, DBG_XFER = 5, DBG_INFO = 6 };   /* DBG_OP_GET: p1 */
enum { DBG_PATH = 1, DBG_TEXT = 2, DBG_WDATA = 0x21 };    /* DBG_OP_PUT: p1 */
enum { DBG_J_IDLE, DBG_J_QUEUED, DBG_J_RUNNING, DBG_J_DONE, DBG_J_FAILED };   /* job states (STATUS p1 bits 8..15) */
enum {                                      /* job results (STATUS p2) */
    DBG_E_OK, DBG_E_PATH, DBG_E_OPEN, DBG_E_IO, DBG_E_SIZE, DBG_E_CRC, DBG_E_RENAME, DBG_E_STATE, DBG_E_TIMEOUT,
    DBG_E_CARD, DBG_E_RESTART, DBG_E_STEPS, DBG_E_BUSY   /* DBG_E_RESTART: kept (core 1.40-1.42: no restart) */
};
enum {                                      /* HELLO's p4 */
    DBG_F_RING = 1, DBG_F_PREV = 2, DBG_F_WRITE = 4, DBG_F_NO_CARD = 8, DBG_F_PTP = 16, DBG_F_BUFFERS = 32,
    DBG_F_RESTART = 64,                     /* RESTART restarts the camera */
    DBG_F_SHOOT = 128,                      /* SHOOT: the test scenario over USB */
    DBG_F_INFO = 256,                       /* GET INFO: the build, the run */
    DBG_F_HANDS_FREE = 512                  /* SHOOT.RUN's hands_free = on: USB held as charge only, a restart at the end */
};
#define DBG_STAGE 16384                     /* the most a GET of BANK or MEM sends */
#define DBG_XFER_SIZE 65536                 /* the job buffer: READ, LIST, SNAP, WDATA pieces */
#define DBG_PATH_MAX 63
#define DBG_TEXT_MAX 255                    /* PUT TEXT: the steps of SHOOT START */
#define DBG_INFO_MAX 512                    /* GET INFO: key=value lines */
#define DBG_LIST_REC 64                     /* LIST: {u32 attributes, u32 size, u32 made, u32 changed (DOS date << 16 | time),
                                               char name[48]} */
#define DBG_SNAP_TASK 92                    /* SNAP: a task, the fields of drv_task in their order */
#define DBG_SNAP_OBJ 16
#define DBG_SNAP_HEAD 36

/* core */
#ifndef OMK_DEBUG
#define dbg_setup(run) ((void)0)
#define dbg_start() ((void)0)
#define dbg_service(now) ((void)0)
#define dbg_tick(tick) (tick)
#define dbg_ended() ((void)0)
#define core_card_ok() 1
#else
void dbg_setup(const char *run);            /* after the config, before the log's first line */
void dbg_start(void);                       /* after the log and the store: buffers, the run before, PTP */
void dbg_service(u32 now);                  /* core loop, every pass while the camera runs */
u32 dbg_tick(u32 tick);                     /* the core loop's next wait: shorter while a window or a job is on */
void dbg_ended(void);                       /* the run reached the end of a power-off */
int core_card_ok(void);                     /* 0 - core writes nothing to the card ([debug] card_writes = off) */
#endif

#endif

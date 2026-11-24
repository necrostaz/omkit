#ifndef HOST_DRV_H
#define HOST_DRV_H

extern u32 host_ticks;                  /* microseconds, like drv_ticks on the camera */
extern u32 host_cpu;                    /* what drv_cpu returns */
extern int host_task;                   /* what drv_task_current returns: the "task" the test runs as */
extern u32 host_region_size;            /* arena size before mem_init, at most 1 MiB */
extern u32 host_fw_total[3], host_fw_out[3];   /* firmware heap: blocks per class and blocks handed out */
extern const char *host_dir;
extern int host_canvas_ok;               /* 0 - no screen */
extern u32 host_prof, host_prof_seq, host_prof_ui, host_prof_base, host_prof_installs;   /* drv_prof_* */
extern u32 host_prof_takes;              /* drv_prof_stats: the pictures counted */
extern int host_prof_install_rc;
extern int host_power_pending;            /* 1 - the firmware lever flag is set */
extern int host_canvas_evf;              /* 1 - viewfinder: canvas 639 x 479 in the same buffers */
extern unsigned char host_canvas[2][1152 * 480];
extern int host_install_rc;              /* what drv_ev_install returns */
extern u32 host_installs[8];             /* how many times the event hook was installed */
extern u32 host_fopens;                  /* drv_fopen calls */
extern int host_no_card;                 /* 1 - drv_fopen fails, as without a card */
extern int host_wakes;                   /* drv_wake calls */
extern int host_isp_ok, host_isp_busy;   /* drv_isp_install works; drv_isp_set answers -2 this many times */
extern u32 host_isp_stock;               /* drv_isp_set(0, 0) calls */

/* The debug service */
typedef struct {
    u32 code, phase;
    drv_ptp_fn fn;
} host_ptp_op;
extern unsigned char host_keep[0x20080];
extern u32 host_keep_size;
extern host_ptp_op host_ptp[8];
extern u32 host_ptp_n;
extern int host_ptp_installs;
extern unsigned char host_ptp_out[1 << 17];
extern u32 host_ptp_out_n, host_ptp_total, host_ptp_sends;
extern const unsigned char *host_ptp_in;
extern u32 host_ptp_in_n;
extern unsigned char host_mem[4096];
extern drv_task host_tasks[4];
extern u32 host_ntasks, host_app;
extern int host_ui;
extern int host_in_task;
extern int host_find;                   /* 1 - drv_findfirst lists the host folder */

#endif

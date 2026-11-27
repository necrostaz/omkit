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
extern u32 host_removes, host_inplace;   /* drv_remove calls; drv_fopen calls in r+ (a rewrite in place) */
extern drv_date_t host_date;            /* what drv_date gives */
void host_files_stale(void);              /* every file open now fails from now on (0x9414) */
extern int host_unmounted;                /* drv_card_mounted gives 0 */
extern int host_fclose_rc;               /* drv_fclose's answer */
extern int host_no_card;                 /* 1 - drv_fopen fails, as without a card */
extern int host_wakes;                   /* drv_wake calls */
extern int host_isp_ok, host_isp_busy;   /* drv_isp_install works; drv_isp_set answers -2 this many times */
extern u32 host_isp_stock;               /* drv_isp_set(0, 0) calls */

extern int host_prev_ended;              /* what core_prev_ended returns */
extern u32 host_opt_level, host_opt_arms;    /* drv_optics_install's level; drv_optics_arm calls */
extern drv_optics_info host_opt;         /* what drv_optics_service gives (armed: set by drv_optics_arm) */
extern void (*host_opt_on_arm)(void);    /* called by drv_optics_arm */
extern u32 host_opt_sub;                 /* the substage drv_optics_sub was given last */
extern u32 host_opt_loop, host_opt_sleep; /* what drv_optics_loop was given last */
extern u32 host_opt_crumb[DRV_OPTICS_CRUMB];
extern int host_opt_worker_rc;           /* what drv_optics_worker answers (0 - running) */
extern u32 host_opt_workers;             /* drv_optics_worker calls */
extern int host_ui_state;                /* what drv_ui_probe answers (0 - a lasting state) */   /* the crumbs: [0] the magic, the rest as the driver's */
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
extern u32 host_keys;
extern int host_in_task;
extern int host_find;                   /* 1 - drv_findfirst lists the host folder */
extern int host_restarts;               /* drv_restart_request calls */
extern u32 host_fopen_us;               /* each drv_fopen moves the clock on by this (0: no time passes) */

/* The test scenario without hands */
extern u32 host_stuck_since, host_k3;
extern int host_usb_install_rc, host_hfw_rc;
extern u32 host_usb_mode, host_usb_hits, host_usb_choice, host_usb_factor, host_usb_link, host_usb_installs;
extern u32 host_ok_presses, host_hfw_posts, host_resets, host_reset_cause;
void host_usb_take(u32 choice);           /* the firmware applies a USB choice (the hook's place) */

#endif

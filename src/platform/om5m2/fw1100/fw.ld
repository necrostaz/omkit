/* OM-5 Mark II 1.1 firmware functions for the thunks in thunks.s */
f_fopen = 0x606aa3bd;
/* fopen's body: d0 flags (u16), a0 path, a1 mode -> a0 file; fopen itself is clr d0 and a call of it with
 * [d2,d3,a2,a3],44. Flag 0x10: the path goes through the long-name walk - 0x606ba3ef tests it (btst 16 at 0x606ba3ff),
 * widens the bytes to UTF-16 (up to 260) and opens through 0x606bd524 -> 0x606bd2f9, the walk findfirst uses for its mask */
f_fopen_flags = 0x606ab771;
f_fread = 0x606aa54e;
f_fwrite = 0x606aa444;
f_fclose = 0x606aa3c6;
f_findfirst = 0x606ab917;                 /* a0 mask "X:\DIR\*.EXT", a1 entry (280 bytes) -> d0 handle, -1 none */
f_findnext = 0x606ab9b6;                  /* d0 handle, a0 entry -> d0 0, -1 the end */
f_findclose = 0x606aba1c;                 /* d0 handle */
f_fflush = 0x606aa4d2;
f_remove = 0x606aa784;
f_rename = 0x606aa6ba;                  /* a0 old path, a1 new path -> d0 0; request 0x10009 */
f_mkdir = 0x606aa81f;
f_exists = 0x606ab659;
f_dcache_inv = 0x608442e6;              /* invalidate a data cache range, L1 and second level, this CPU */
f_ev_create = 0x60051ace;               /* acre_Event1 */
f_ev_wait = 0x60051143;                 /* OlyEvWt */
f_ev_set = 0x60051078;                  /* OlyEvSt */
f_ev_clear = 0x6005123a;                /* OlyEvCl */
f_fseek = 0x606aa646;
f_ftell = 0x606aa5dc;
f_sleep = 0x60051a2b;
f_setting_read = 0x602f4c5b;
/* the test scenario (drv.c drv_shutter): API 0xD0002, a key of the key task's image (d0 the key's event, d1 state 0 pressed,
 * 1 released; the shell's "ui key skey" -> 0x601310b8); posting an event to the application's queue (d0 event, d1 0) */
f_key_api = 0x6032b57f;
f_app_event = 0x60046624;
f_lock = 0x6006e645;
f_unlock = 0x6006e68e;
f_phyalloc = 0x60023818;                /* scheap_phyalloc: uncached heap, one for all CPUs */
f_phyfree = 0x6002384a;
pm_back = 0x602d7969;                   /* past the displaced instructions of handler 111 */
s1_back = 0x60323a6a;                   /* past the displaced instruction of API 0x80001 */
/* power hooks: past the displaced instructions */
off_back = 0x6089e779;                  /* CameraCommon::shutDown 0x6089e774 */
card_back = 0x601b326d;                 /* card unmount 0x601b3267 */
sleep_back = 0x6089e868;                /* CameraCommon::startSleep 0x6089e863 */
gate_back = 0x60325f87;                 /* API 0xC0001 0x60325f80 */
wake_back = 0x6089841e;                 /* sleepForSystem after API 0xC0001 returned, 0x60898419 */
/* picture profiles, pictprof.c: UI settings object methods, availability handler of 111, shooting reader */
orig_ui_write = 0x608efc53;
orig_ui_write4 = 0x608efeea;
orig_ui_read = 0x608ef257;
orig_avail = 0x602d7964;
take_back = 0x602c8adb;                 /* past mov 29490,a2; add d3,a2 at 0x602c8ad6 */
alloc_back = 0x6042667c;                /* past mov d0,d3; mov 53649,d0 at 0x60426675 */
f_mode_changed = 0x602dd18d;            /* stock callback of setting 111: rebuild the picture mode (calls convention) */
column_back = 0x60e376d5;               /* the column function's common part after the list and count (0x60e375d2) */
lva_back = 0x6033e139;                  /* past mov (8,sp),a0; mov (4,sp),a1; movbu (129,a0),d0 at 0x6033e131 */
/* graphic profile editors, pictprof.c */
item_level_fw = 0x60e4af24;             /* handler of the editor item Color/Level 5373, as its dispatcher stub 0x60e49e3a calls it */
item_back = 0x60e49e4a;                 /* the item dispatcher after the handlers */
c3d_back = 0x60ccf4fe;                  /* past mov a0,a2; mov 0x60f580a8,a1 at the Color Creator renderer 0x60ccf4f7 */
item_vivid_fw = 0x60e4ac9b;             /* handler of the editor item Color/Vivid 5374, as its stub 0x60e49e31 calls it */
item_curve_fw = 0x60e4b018;             /* handler of the editor item tone curve 5375, as its stub 0x60e49e43 calls it */
item_cc_handler = 0x60e4ab3a;            /* handler of the Color Creator item 5029, as the branch item_cc_fw calls it */
item_cc_fw = 0x60e49e1f;                /* the item dispatcher's branch of the Color Creator item 5029 */
c3d_swap = 0x60ccf0f8;                  /* the C3D renderers' eglSwapBuffers(display, surface) */
/* OpenVG 1.1, direct entries: table 0x60293bc0 address + the movm/add prologue */
vg_create_path = 0x613dab09;
vg_clear_path = 0x613dab37;
vg_append = 0x613dabd5;
vg_draw_path = 0x613dacf8;
vg_destroy_path = 0x613dab53;
vg_create_paint = 0x613dad41;
vg_set_color = 0x613dadc0;
vg_set_paint = 0x613dad89;
vg_destroy_paint = 0x613dad5a;
vg_seti = 0x613da315;
vg_setfv = 0x613da331;
vg_setf = 0x613da2f9;
/* loading profiles from outside, import.c */
imp_ptp_reg = 0x614c0156;               /* PTP vendor operation registration: d0 code, d1 phase, a0 handler, a1 0 */
imp_reg_back = 0x614ba926;              /* past mov 0x970c,d0 at 0x614ba920 */
ptp_send = 0x614c02d6;                  /* PTP data to the computer: d0 piece, d1 total, a0 buffer -> d0 (u16) 1 */
ptp_recv_len = 0x614c0399;              /* PTP data from the computer: the container header -> d0 its data length */
ptp_recv = 0x614c040f;                  /* PTP data from the computer: d0 length, a0 buffer -> d0 (u16) 1 */
imp_parse_ret = 0x6160a850;             /* return address of the ID lookup in the value parse of a load (0x6160a84b) */
imp_id_back = 0x6160a69a;               /* past mov 0x6160b888,a0 at the ID lookup's entry 0x6160a694 */
imp_line_done = 0x6160a135;             /* the load's tail of a line */
imp_apply_back = 0x6160a08d;            /* past mov (72,sp),d0; cmp 256,d0 at 0x6160a087 */
imp_snap_put = 0x602f5243;              /* Current: the snapshot (d0 id, d1 value) */
imp_work_put = 0x602f5396;              /* Current: the working array */
vg_load_identity = 0x613da235;
vg_clear = 0x613daad8;
/* ISP colour table override, ispdrv.c */
isp_w_back = 0x6037c51f;                /* past mov a1,d1; mov a0,d3; mov d3,(sp) at the register writer 0x6037c519 */
isp_b_back = 0x603e8fbf;                /* past mov a0,d3; mov d3,a3; inc4 a3 at the colour builder 0x603e8fba */
/* the look's grain, graindrv.c: S and A */
grain_sel_back = 0x603ecda8;            /* the selector 0x603ecda1 past mov a0,a2; inc4 a2; mov (a2),a1; movbu (16,a1),d0 */
grain_set_back = 0x6036fc79;            /* past mov (a2),a0; mov (24,a0),a0 at 0x6036fc74 */
/* the look's grain, graindrv.c: the texture */
grain_h2_back = 0x603a5319;             /* the tile node builder past mov sp,a0; add 164,a0 at 0x603a5314 */
grain_h3_back = 0x6038a5d6;             /* the cleanup gate past mov (11308,a0),d0; cmp 12,d0 at 0x6038a5d0 */
grain_wl_back = 0x6038a537;             /* the planner selector 0x6038a4f0 past mov (11308,a2),d0; cmp 12,d0 at 0x6038a531 */
grain_wl_after = 0x6038a549;            /* ... after its planner call: d0 the planner's result, a2 h, a3 P */
grain_det_back = 0x6036fbc0;            /* the pass setup's check 0x6036fbbb past mov (24,a0),a1; mov 1,d2 */
fw_art_plan = 0x6038c072;               /* the ART planner (-> 0x6038d263), called as 0x6038a53a does: call ...,[],4, a0 h */
fw_tex_build = 0x603a3bdb;              /* texture builder: a0 P, a1 parameters -> a0 record; [d2,d3,a2,a3],28 */
fw_tex_release = 0x603a452a;            /* texture release: a0 P, a1 record; [a2,a3],12 */

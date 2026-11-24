# Thunks to OM-5 Mark II 1.1 firmware functions. GCC: the first two words in d0, d1, the rest at (12,sp), (16,sp).
# Firmware: pointers in a0, a1, integers in d0, d1; registers and frame are encoded in the call instruction and must match the function's ret.
# call writes the return address to (sp), so the thunk reserves its own slot. The addresses are in fw.ld.
	.text
	.global	_drv_findfirst, _drv_findnext, _drv_findclose
	.global	_drv_fopen_long
	.global	_start, _drv_fopen, _drv_fread, _drv_fwrite, _drv_fclose, _drv_sleep, _drv_ticks, _drv_setting_read
	.global	_drv_ev_create, _drv_ev_wait, _drv_ev_set, _drv_ev_clear
	.global	_drv_mkdir, _drv_exists, _drv_dcache_inv_fw, _drv_fflush, _drv_remove, _drv_fseek, _drv_ftell, _drv_cpu, _drv_lock, _drv_unlock, _drv_fwheap_alloc, _drv_fwheap_free
	.global	_drv_tramp_pm, _drv_orig_pm, _drv_tramp_s1
	.global	_drv_tramp_off, _drv_tramp_card, _drv_tramp_sleep, _drv_tramp_gate, _drv_tramp_wake
	.global	_drv_tramp_take, _drv_ui_read, _drv_ui_write, _drv_ui_write4, _drv_ui_avail
	.global	_drv_tramp_alloc, _drv_tramp_lvr, _drv_tramp_lva, _drv_fw_mode_changed
	.global	_drv_item_level, _drv_item_vivid, _drv_item_curve, _drv_item_cc, _drv_tramp_c3d, _drv_c3d_swap
	.global	_drv_vg_create_path, _drv_vg_clear_path, _drv_vg_append, _drv_vg_draw_path, _drv_vg_destroy_path, _drv_vg_create_paint
	.global	_drv_vg_set_color, _drv_vg_set_paint, _drv_vg_destroy_paint, _drv_vg_seti, _drv_vg_setfv, _drv_vg_setf, _drv_vg_load_identity, _drv_vg_clear
	.global	_drv_column_color, _drv_column_look, _drv_mf0, _drv_mf1, _drv_mf2, _drv_mf3, _drv_mf4, _drv_mf5, _drv_mf6, _drv_mf7

	.section .text.start,"ax"
_start:
	mov	sp,a0
	mov	___stack_top,a1			# own stack in the image: the loader's stack is only 2 KB
	mov	a1,sp
	add	-8,sp
	mov	a0,(4,sp)
	call	_core_main,[],0
	mov	(4,sp),a0
	mov	a0,sp
	rets

	.text
_drv_fopen:
	mov	d0,a0
	mov	d1,a1
	add	-4,sp
	call	f_fopen,[],4
	add	4,sp
	mov	a0,d0
	rets

# A file by its long name (FAT LFN): d0 path, d1 mode. As the firmware's fopen 0x606aa3bd, with flag 0x10 in place of 0
_drv_fopen_long:
	mov	d0,a0
	mov	d1,a1
	add	-4,sp
	call	fopen_long,[],4
	add	4,sp
	mov	a0,d0
	rets

fopen_long:
	mov	16,d0
	call	f_fopen_flags,[d2,d3,a2,a3],44
	ret	[],4

_drv_fread:
	mov	d0,a0
	mov	d1,d0
	mov	(12,sp),d1
	mov	(16,sp),a1
	add	-4,sp
	call	f_fread,[d2,d3,a2,a3],24
	add	4,sp
	rets

_drv_fwrite:
	mov	d0,a0
	mov	d1,d0
	mov	(12,sp),d1
	mov	(16,sp),a1
	add	-4,sp
	call	f_fwrite,[d2,d3,a2,a3],24
	add	4,sp
	rets

_drv_fclose:
	mov	d0,a0
	add	-4,sp
	call	f_fclose,[d2,a2,a3],16
	add	4,sp
	rets

# Files of a folder (the firmware's findfirst/findnext/findclose, as its play_rom_chk_bgm uses them): d0 mask, d1 entry
_drv_findfirst:
	mov	d0,a0
	mov	d1,a1
	add	-4,sp
	call	f_findfirst,[d2,a2,a3],32
	add	4,sp
	rets

# d0 handle, d1 entry
_drv_findnext:
	mov	d1,a0
	add	-4,sp
	call	f_findnext,[d2,a2,a3],16
	add	4,sp
	rets

# d0 handle
_drv_findclose:
	add	-4,sp
	call	f_findclose,[d2,a2],12
	add	4,sp
	rets

# Data cache of this CPU, range: d0 address, d1 length -> d0 0. The firmware function disables interrupts itself, drops the
# lines of the first and the second level without writing them back (0x608442e6)
_drv_dcache_inv_fw:
	add	-4,sp
	call	f_dcache_inv,[d2,d3,a2,a3],28
	add	4,sp
	rets

# RTOS event flag through the firmware wrappers (conventions from the call sites
# 0x6005ff07 and 0x6005ff89). acre_Event1: a0 name, d0 attribute, d1 initial pattern -> d0 the flag id (negative: error)
_drv_ev_create:
	mov	d0,a0
	mov	4,d0
	clr	d1
	add	-4,sp
	call	f_ev_create,[d2,d3,a2,a3],36
	add	4,sp
	exth	d0
	rets

# OlyEvWt: d0 id, d1 pattern, a0 -> the pattern seen, a1 1 (any bit), (4,sp) timeout in ms -> d0 0, or an error/timeout code.
# Our frame: (0,sp) the slot call writes its return address to, (4,sp) the timeout, (8,sp) the result word
_drv_ev_wait:
	add	-12,sp
	mov	d1,(4,sp)
	exth	d0
	mov	1,d1
	mov	sp,a0
	add	8,a0
	mov	1,a1
	call	f_ev_wait,[d2,d3,a2,a3],52
	add	12,sp
	rets

# OlyEvSt: d0 id, d1 bits to set
_drv_ev_set:
	exth	d0
	mov	1,d1
	add	-4,sp
	call	f_ev_set,[d2,d3,a2],40
	add	4,sp
	rets

# OlyEvCl: d0 id, d1 the mask that stays (ITRON clr_flg: pattern &= d1)
_drv_ev_clear:
	exth	d0
	mov	-2,d1
	add	-4,sp
	call	f_ev_clear,[d2,d3,a2],28
	add	4,sp
	rets

# mkdir: a0 path, d0 0 (as the firmware's own calls, e.g. 0x606a783e) -> d0. exists: a0 path -> d0, not 0 if it is there
# (the firmware calls it before mkdir: 0x601100e6, 0x6019ea4f). 
_drv_mkdir:
	mov	d0,a0
	clr	d0
	add	-4,sp
	call	f_mkdir,[d2,a2,a3],28
	add	4,sp
	rets

_drv_exists:
	mov	d0,a0
	add	-4,sp
	call	f_exists,[],8
	add	4,sp
	rets

# remove: a0 path -> d0
_drv_remove:
	mov	d0,a0
	add	-4,sp
	call	f_remove,[d2,a2,a3],24
	add	4,sp
	rets

# fseek from the start: the firmware numbers the origins its own way, 2 is "from the start"
_drv_fseek:
	mov	d0,a0
	mov	d1,d0
	mov	2,d1
	add	-4,sp
	call	f_fseek,[d2,d3,a2,a3],20
	add	4,sp
	rets

_drv_ftell:
	mov	d0,a0
	add	-4,sp
	call	f_ftell,[d2,a2,a3],16
	add	4,sp
	rets

_drv_fflush:
	mov	d0,a0
	add	-4,sp
	call	f_fflush,[d2,a2,a3],16
	add	4,sp
	rets

_drv_sleep:
	add	-4,sp
	call	f_sleep,[d2],8
	add	4,sp
	rets

# scheap_phyalloc: size in d0, address in a0; scheap_phyfree: address in a0
_drv_fwheap_alloc:
	add	-4,sp
	call	f_phyalloc,[],4
	add	4,sp
	mov	a0,d0
	rets

_drv_fwheap_free:
	mov	d0,a0
	add	-4,sp
	call	f_phyfree,[],4
	add	4,sp
	rets

_drv_setting_read:
	add	-4,sp
	call	f_setting_read,[],4
	add	4,sp
	rets

_drv_lock:
	clr	d0
	add	-4,sp
	call	f_lock,[d2],8
	add	4,sp
	rets

_drv_unlock:
	add	-4,sp
	call	f_unlock,[d2],8
	add	4,sp
	rets

_drv_cpu:
	movhu	(0x8050),d0
	rets

_drv_ticks:
	mov	(0x9890014c),d1
	mov	0x3fffffff,d0
	sub	d1,d0
	rets

# ---------------------------------------------------------------------------------------------------------------
# Event hooks. The mechanism was verified on the camera in core 0.11.
# Hook registers for C (hook_regs in drv.c): d0, d1, a0, a1, the stack argument (4,sp) on entry, the result.

# Entry of availability handler 111 0x602d7964: the firmware asks whether a mode (a1) is allowed. The whole function
# is executed by C: module handlers and, through the chain, the original via _drv_orig_pm. Result in d0, return to the caller.
_drv_tramp_pm:
	add	-24,sp
	mov	d0,(0,sp)
	mov	d1,(4,sp)
	mov	a0,(8,sp)
	mov	a1,(12,sp)
	mov	(28,sp),d0			# caller's argument: (4,sp) on entry
	mov	d0,(16,sp)
	mov	sp,a0				# &hook_regs: sp can only be copied into an address register
	mov	a0,d0
	mov	(8,a0),a0			# original a0: movm will save it
	movm	[d2,d3,a2,a3,other],(sp)
	mov	psw,d1
	add	-4,sp
	mov	d1,(sp)
	add	-32,sp
	call	_drv_hook_pm,[],0
	add	32,sp
	mov	(sp),d1
	add	4,sp
	mov	d1,psw
	movm	(sp),[d2,d3,a2,a3,other]
	mov	(20,sp),d0			# result
	add	24,sp
	rets

# Original function 0x602d7964 by hook_regs (d0 from C): the same registers and stack argument as the firmware caller had
_drv_orig_pm:
	movm	[d2,d3,a2,a3],(sp)
	mov	d0,a2
	add	-8,sp				# (sp) is the return address written by calls; (4,sp) is the argument
	mov	(16,a2),d0
	mov	d0,(4,sp)
	mov	(0,a2),d0
	mov	(4,a2),d1
	mov	(8,a2),a0
	mov	(12,a2),a1
	calls	orig_pm_body
	add	8,sp
	movm	(sp),[d2,d3,a2,a3]
	rets
orig_pm_body:
	.byte	0xcf,0x80,0xf8,0xfe,0xfc	# displaced: movm [d2],(sp); add -4,sp
	jmp	pm_back

# Entry of API function 0x80001 0x60323a64 (accompanies S1): notification only, then the original continues
_drv_tramp_s1:
	movm	[d2,d3,a2,a3,other],(sp)
	mov	psw,d0
	add	-4,sp
	mov	d0,(sp)
	add	-32,sp
	call	_drv_hook_s1,[],0
	add	32,sp
	mov	(sp),d0
	add	4,sp
	mov	d0,psw
	movm	(sp),[d2,d3,a2,a3,other]
	.byte	0xfc,0xcc,0x01,0x00,0x08,0x00	# displaced: mov 0x80001,d0
	jmp	s1_back

# ---------------------------------------------------------------------------------------------------------------
# Power hooks: drv_hook_power(kind, arg) with every register and psw kept, then the displaced
# instructions and a jump back. arg: the firmware register named in each hook. The gates may sleep in C (dly_tsk).
	.macro	POWER_HOOK kind, arg
	movm	[d2,d3,a2,a3,other],(sp)
	.ifnc	\arg,d1			# mov d1,d1 does not exist: its encoding is mov imm8,d1
	mov	\arg,d1
	.endif
	mov	psw,d0
	add	-4,sp
	mov	d0,(sp)
	add	-32,sp
	mov	\kind,d0
	call	_drv_hook_power,[],0
	add	32,sp
	mov	(sp),d0
	add	4,sp
	mov	d0,psw
	movm	(sp),[d2,d3,a2,a3,other]
	.endm

# CameraCommon::shutDown 0x6089e774: every power-off through the application, d1 - reason
_drv_tramp_off:
	POWER_HOOK 1, d1
	.byte	0xcf,0xf0,0xf8,0xfe,0xe4	# displaced: movm [d2,d3,a2,a3],(sp); add -28,sp
	jmp	off_back

# 0x601b3267: the card is about to be unmounted (message 53 to the file system task); gate
_drv_tramp_card:
	POWER_HOOK 2, d0
	.byte	0xfc,0xdc,0xc9,0x0a,0x21,0x60	# displaced: mov 0x60210ac9,a0
	jmp	card_back

# CameraCommon::startSleep 0x6089e863: sleep is being prepared
_drv_tramp_sleep:
	POWER_HOOK 3, d0
	.byte	0xcf,0xa0,0xf8,0xfe,0xf8	# displaced: movm [d2,a2],(sp); add -8,sp
	jmp	sleep_back

# API 0xC0001 0x60325f80: the sleep task takes over, the card loses power; gate. d0 - sleep length, ms
_drv_tramp_gate:
	POWER_HOOK 4, d0
	.byte	0x82,0xfc,0xcc,0x01,0x00,0x0c,0x00	# displaced: mov d0,d2; mov 0xc0001,d0
	jmp	gate_back

# 0x60898419 in sleepForSystem: API 0xC0001 returned, d2 - its result
_drv_tramp_wake:
	POWER_HOOK 5, d2
	.byte	0x00,0x42,0x14,0x8f,0x23	# displaced: clr d0; mov d0,(20,sp); mov 35,d3
	jmp	wake_back

# ---------------------------------------------------------------------------------------------------------------
# Picture profiles, pictprof.c. 

# Shooting parameter reader 0x602c8139 at 0x602c8ad6: d3 = S. drv_take_filter(S) with every register and psw kept
_drv_tramp_take:
	movm	[d2,d3,a2,a3,other],(sp)
	mov	psw,d0
	add	-4,sp
	mov	d0,(sp)
	add	-32,sp
	mov	d3,d0
	call	_drv_take_filter,[],0
	add	32,sp
	mov	(sp),d0
	add	4,sp
	mov	d0,psw
	movm	(sp),[d2,d3,a2,a3,other]
	.byte	0x26,0x32,0x73,0xf1,0x6e	# displaced: mov 29490,a2; add d3,a2
	jmp	take_back

# UI settings object, method +8 (called with calls): d0 id, a1 where. The original first, then drv_ui_read_fix(id, where,
# return address) if it succeeded (d0 = 0). The original takes no stack arguments (checked in the emulator)
_drv_ui_read:
	movm	[d2,d3,a2,a3],(sp)
	add	-12,sp				# (0,sp) the slot calls writes to, (4,sp) a0, (8,sp) d1; return address at (28,sp)
	mov	d0,d2
	mov	a1,a2
	mov	a0,(4,sp)
	mov	d1,(8,sp)
	mov	(28,sp),d1			# who reads: the picture mode screen's reads of effective ids go to the profile's cells
	add	-32,sp
	call	_drv_ui_read_id,[],0
	add	32,sp
	mov	(4,sp),a0
	mov	(8,sp),d1
	mov	a2,a1
	mov	orig_ui_read,a3
	calls	(a3)
	mov	d0,d3
	cmp	0,d3
	bne	1f
	mov	d2,d0
	exthu	d0
	mov	a2,d1
	add	-32,sp
	mov	(60,sp),a0			# argument 3: who reads (the return address, now at 28 + 32)
	mov	a0,(12,sp)
	call	_drv_ui_read_fix,[],0
	add	32,sp
1:	mov	d3,d0
	ret	[d2,d3,a2,a3],28

# Method +0: d0 id, d1 kind (0 - a value), a1 value. For kind 0 the value goes through drv_ui_write_fix(id, value); then the
# original is called (not jumped to: it takes no stack arguments) and, after it returned, drv_ui_after_write()
# rebuilds the picture mode if the profile changed. Frame: 16 saved + 24 locals, the result at (20,sp). First of all
# drv_ui_swallow(id, kind, value): 1 - the film look's dial cells, the write is not done and reported done (d0 = 0)
_drv_ui_write:
	movm	[d2,d3,a2,a3],(sp)
	add	-24,sp
	mov	d0,(4,sp)
	mov	d1,(8,sp)
	mov	a0,(12,sp)
	mov	a1,(16,sp)
	add	-32,sp
	mov	a1,(12,sp)			# argument 3: the value
	call	_drv_ui_swallow,[],0
	add	32,sp
	cmp	0,d0
	beq	2f
	clr	d0
	ret	[d2,d3,a2,a3],40
2:	mov	(4,sp),d0
	mov	(8,sp),d1
	add	-32,sp
	call	_drv_ui_write_id,[],0		# the H&S screen with a profile: its tone cells (decision R7)
	add	32,sp
	mov	d0,(4,sp)
	mov	(8,sp),d1
	mov	(16,sp),a1
	cmp	0,d1
	bne	1f
	exthu	d0
	mov	a1,d1
	add	-32,sp
	call	_drv_ui_write_fix,[],0
	add	32,sp
	mov	d0,(16,sp)
1:	mov	(4,sp),d0
	mov	(8,sp),d1
	mov	(12,sp),a0
	mov	(16,sp),a1
	mov	orig_ui_write,a3
	calls	(a3)
	mov	d0,(20,sp)
	add	-32,sp
	call	_drv_ui_after_write,[],0
	add	32,sp
	mov	(20,sp),d0
	ret	[d2,d3,a2,a3],40

# Method +4: d0 id, a1 value; always a value. Same as +0
_drv_ui_write4:
	movm	[d2,d3,a2,a3],(sp)
	add	-24,sp
	mov	d0,(4,sp)
	mov	d1,(8,sp)
	mov	a0,(12,sp)
	mov	a1,(16,sp)
	add	-32,sp
	mov	a1,(12,sp)			# argument 3: the value; kind 0
	clr	d1
	call	_drv_ui_swallow,[],0
	add	32,sp
	cmp	0,d0
	beq	2f
	clr	d0
	ret	[d2,d3,a2,a3],40
2:	mov	(4,sp),d0
	add	-32,sp
	call	_drv_ui_write_id,[],0
	add	32,sp
	mov	d0,(4,sp)
	mov	(16,sp),a1
	exthu	d0
	mov	a1,d1
	add	-32,sp
	call	_drv_ui_write_fix,[],0
	add	32,sp
	mov	d0,(16,sp)
	mov	(4,sp),d0
	mov	(8,sp),d1
	mov	(12,sp),a0
	mov	(16,sp),a1
	mov	orig_ui_write4,a3
	calls	(a3)
	mov	d0,(20,sp)
	add	-32,sp
	call	_drv_ui_after_write,[],0
	add	32,sp
	mov	(20,sp),d0
	ret	[d2,d3,a2,a3],40

# Availability handler of 111 (called with calls, a1 = mode): profile modes 54..59 are allowed (0), others - the original
_drv_ui_avail:
	mov	a1,d1
	exthu	d1
	add	-54,d1
	cmp	6,d1
	bcc	1f
	clr	d0
	rets
1:	jmp	orig_avail

# Allocator setup 0x60426675: d0 = raw 111 just read. drv_alloc_filter(&d0) may change it; every other register and psw are
# kept. The word at (0,sp) carries d0 in and out; a0 is saved at (4,sp) only to make the pointer
_drv_tramp_alloc:
	add	-8,sp
	mov	d0,(0,sp)
	mov	a0,(4,sp)
	mov	sp,a0
	mov	a0,d0
	mov	(4,a0),a0
	movm	[d2,d3,a2,a3,other],(sp)
	mov	psw,d1
	add	-4,sp
	mov	d1,(sp)
	add	-32,sp
	call	_drv_alloc_filter,[],0
	add	32,sp
	mov	(sp),d1
	add	4,sp
	mov	d1,psw
	movm	(sp),[d2,d3,a2,a3,other]
	mov	(0,sp),d0
	add	8,sp
	.byte	0x83,0xfc,0xcc,0x91,0xd1,0x00,0x00	# displaced: mov d0,d3; mov 53649,d0
	jmp	alloc_back

# Live view (6.3) and pictprof.c (LVR, LVA). They may run on CPU 2: no C, no stack,
# only drv_lv_shared through its uncached alias (offsets as LV_* in pictprof.c).
#
# LVR replaces the leaf 0x6034f966 (mov (0xa26eca5c),a0; movbu (a0),d0; retf [],0): the same result, except that Natural (2)
# becomes LV_MODE while it is not 0. Only d0 and a0 change, as in the original; retf returns through mdr, untouched
_drv_tramp_lvr:
	mov	(0xa26eca5c),a0
	movbu	(a0),d0
	cmp	2,d0
	bne	1f
	mov	_drv_lv_shared+0x40000000,a0
	movbu	(4,a0),d0
	cmp	0,d0
	beq	2f
	mov	(52,a0),d0			# LV_READS
	inc	d0
	mov	d0,(52,a0)
	movbu	(4,a0),d0			# LV_MODE again (d0 held the counter): the core task may have cleared it
	cmp	0,d0				# since the check above - then Natural, never mode 0
	beq	2f
	retf	[],0
2:	mov	2,d0
1:	retf	[],0

# The copy of LVA: the block's values into the context's parameter block. a0 the block, a1 the context; d0 changes
	.macro	LVA_COPY
	clr	d0
	movbu	d0,(752,a1)			# saturation 0
	mov	1,d0
	movbu	d0,(754,a1)			# gradation 1 (normal)
	movbu	(6,a0),d0
	movbu	d0,(753,a1)
	movbu	(7,a0),d0
	movbu	d0,(756,a1)
	movbu	(8,a0),d0
	movbu	d0,(734,a1)
	movbu	(8,a0),d0
	movbu	d0,(263,a1)
	movbu	(9,a0),d0
	movbu	d0,(735,a1)
	movbu	(9,a0),d0
	movbu	d0,(264,a1)
	movbu	(10,a0),d0
	movbu	d0,(705,a1)
	movbu	(11,a0),d0
	movbu	d0,(730,a1)
	movbu	(12,a0),d0
	movbu	d0,(731,a1)
	movbu	(13,a0),d0
	movbu	d0,(732,a1)
	movbu	(14,a0),d0
	movbu	d0,(733,a1)
	movbu	(15,a0),d0
	movbu	d0,(792,a1)
	movbu	(16,a0),d0
	movbu	d0,(793,a1)
	movbu	(17,a0),d0
	movbu	d0,(794,a1)
	movhu	(20,a0),d0
	movhu	d0,(706,a1)
	movhu	(22,a0),d0
	movhu	d0,(708,a1)
	movhu	(24,a0),d0
	movhu	d0,(710,a1)
	movhu	(26,a0),d0
	movhu	d0,(712,a1)
	movhu	(28,a0),d0
	movhu	d0,(714,a1)
	movhu	(30,a0),d0
	movhu	d0,(716,a1)
	movhu	(32,a0),d0
	movhu	d0,(718,a1)
	movhu	(34,a0),d0
	movhu	d0,(720,a1)
	movhu	(36,a0),d0
	movhu	d0,(722,a1)
	movhu	(38,a0),d0
	movhu	d0,(724,a1)
	movhu	(40,a0),d0
	movhu	d0,(726,a1)
	movhu	(42,a0),d0
	movhu	d0,(728,a1)
	.endm

# LVA at 0x6033e131, after the context's parameter block was filled: A at (12,sp). d0, d1, a0, a1 are dead here (the displaced
# instructions load a0, a1, d0 next; the callee after them overwrites d1). Copy only when A+260 is our LV_MODE and the block
# is not being written (LV_SEQ even). LV_SEQ is read again after the copy: if the core task wrote the block meanwhile, the
# copy may mix two sets, and it is made once more (no loop: this is a firmware task on any CPU; a second change within
# the copy leaves a mixed set until the next frame, as before)
_drv_tramp_lva:
	mov	(12,sp),a1
	cmp	0,a1
	beq	9f
	mov	_drv_lv_shared+0x40000000,a0
	movbu	(4,a0),d1
	cmp	0,d1
	beq	9f
	movbu	(260,a1),d0
	cmp	d1,d0
	bne	9f
	mov	(0,a0),d1
	btst	1,d1
	bne	9f
	LVA_COPY
	mov	(0,a0),d0			# LV_SEQ again: did the core task write the block during the copy?
	cmp	d1,d0
	beq	8f
# once more, whatever LV_SEQ is now: the newer values; the next frame copies again
	LVA_COPY
8:	mov	(56,a0),d0			# LV_APPLIED
	inc	d0
	mov	d0,(56,a0)
9:	.byte	0x5c,0x08,0x5d,0x04,0xfa,0x40,0x81,0x00	# displaced: mov (8,sp),a0; mov (4,sp),a1; movbu (129,a0),d0
	jmp	lva_back

# The stock callback of 111 (calls convention, no arguments, keeps d2, d3, a2, a3), from C in the GUI task.
# calls writes its return address to (sp): the thunk keeps its own slot
_drv_fw_mode_changed:
	add	-4,sp
	mov	f_mode_changed,a0
	calls	(a0)
	add	4,sp
	rets

# The colour profile column of the editor (pictprof.c): jumped to from the firmware's column function in place of its label
# 0x60e376c3, with its frame as it is: our list of five, then the firmware's common part
_drv_column_color:
	mov	_drv_color_column,a3
	mov	5,d0
	jmp	column_back

# The film look's column (pictprof.c), in place of the slot's label: drv_look_column() gives {count, items}. At the label the
# column function keeps nothing in d0, d1, a0, a1, a2 (the common part loads them first); a3 and d0 are its list and count
_drv_column_look:
	add	-32,sp
	call	_drv_look_column,[],0
	add	32,sp
	mov	d0,a3
	mov	(a3),d0
	inc4	a3
	jmp	column_back

# Monochrome filter matrix handlers (pictprof.c): jumped to from the lookup 0x603e4f7f like its own "mov matrix,a0; retf";
# the matrix through the uncached alias
_drv_mf0:
	mov	_drv_mf_shared+0x40000000+0,a0
	retf	[],0
_drv_mf1:
	mov	_drv_mf_shared+0x40000000+18,a0
	retf	[],0
_drv_mf2:
	mov	_drv_mf_shared+0x40000000+36,a0
	retf	[],0
_drv_mf3:
	mov	_drv_mf_shared+0x40000000+54,a0
	retf	[],0
_drv_mf4:
	mov	_drv_mf_shared+0x40000000+72,a0
	retf	[],0
_drv_mf5:
	mov	_drv_mf_shared+0x40000000+90,a0
	retf	[],0
_drv_mf6:
	mov	_drv_mf_shared+0x40000000+108,a0
	retf	[],0
_drv_mf7:
	mov	_drv_mf_shared+0x40000000+126,a0
	retf	[],0


# Graphic profile editors (editor.c). drv_edp_shared, through the
# uncached alias only: +0 the item handler calls, +4 the Color Creator renderer calls, +8 the CPU of the last renderer call,
# +16 the mode (0 - the native ring).
# Item Color/Level 5373: jumped to by the item dispatcher's jmp (a1) at 0x60e49e03 in place of its stub 0x60e49e3a (a3 the
# panel): the item's own handler as the stub calls it, the values for the drawing (drv_edp_level, GUI task), then the
# "background" of the Color Creator item 0x60e4ab68..0x60e4ab7f (panel+4278+10 = 0xfff8, +12 = 1, panel+17 = 1), so the GUI
# frame orders the Color Creator drawing for this item too, and its Reset hint (+8 as drv_edp_level returns it, +9 = 0,
# as the Color Creator handler 0x60e4ac1d..0x60e4ac4c writes them)
_drv_item_level:
	mov	a3,a0
	mov	a0,a1
	call	item_level_fw,[d2,d3,a2,a3],44
	add	-32,sp
	call	_drv_edp_level,[],0		# a3 is kept by C
	add	32,sp
	mov	4278,a0
	add	a3,a0
	movbu	d0,(8,a0)			# the "Reset OK Hold" hint: 1 - the values differ from the reset ones (0x60e4ac1d..)
	clr	d0
	movbu	d0,(9,a0)
	mov	0xfff8,d0
	movhu	d0,(10,a0)
	mov	1,d0
	movbu	d0,(12,a0)
	movbu	d0,(17,a3)
	jmp	item_back

# Item Color/Vivid 5374, in place of its stub 0x60e49e31: as Color/Level, with its own handler and values
_drv_item_vivid:
	mov	a3,a0
	mov	a0,a1
	call	item_vivid_fw,[d2,d3,a2,a3],68
	add	-32,sp
	call	_drv_edp_vivid,[],0
	add	32,sp
	mov	4278,a0
	add	a3,a0
	movbu	d0,(8,a0)			# the "Reset OK Hold" hint: 1 - the values differ from the reset ones (0x60e4ac1d..)
	clr	d0
	movbu	d0,(9,a0)
	mov	0xfff8,d0
	movhu	d0,(10,a0)
	mov	1,d0
	movbu	d0,(12,a0)
	movbu	d0,(17,a3)
	jmp	item_back

# Item tone curve 5375, in place of its stub 0x60e49e43 (call 0x60e4b018,[],4): as Color/Level
_drv_item_curve:
	mov	a3,a0
	mov	a0,a1
	call	item_curve_fw,[],4
	add	-32,sp
	call	_drv_edp_curve,[],0
	add	32,sp
	mov	4278,a0
	add	a3,a0
	movbu	d0,(8,a0)
	clr	d0
	movbu	d0,(9,a0)
	mov	0xfff8,d0
	movhu	d0,(10,a0)
	mov	1,d0
	movbu	d0,(12,a0)
	movbu	d0,(17,a3)
	jmp	item_back

# Item Color Creator 5029, in place of its stub 0x60e49e1f: our mode off and the stock dial labels (drv_edp_cc), so its own
# ring is drawn; then the stub. The stub uses a3 (kept by C) and writes a0, a1 first
_drv_item_cc:
	add	-32,sp
	call	_drv_edp_cc,[],0		# d0 = 1: the film look's item (a3 is kept by C)
	add	32,sp
	cmp	0,d0
	bne	1f
	jmp	item_cc_fw
1:	mov	a3,a0				# as the branch 0x60e49e1f: the item's handler, then the dispatcher's end
	mov	a0,a1
	call	item_cc_handler,[d2,d3,a2,a3],44
	mov	4278,a0
	add	a3,a0
	clr	d0
	movbu	d0,(8,a0)			# no "Reset OK Hold": the look has nothing to reset (the handler sets it from 615/616)
	movbu	d0,(9,a0)			# nor its button (the handler's kind 2/3; 0 as at 615 = 616 = 0)
	movhu	d0,(2,a0)			# the rear dial's value text ("+-0", widget 0x3dc): empty
	jmp	item_back

# Entry of the Color Creator renderer 0x60ccf4f7 (C3D_MAIN, CPU 1; a0 the 4 bytes of data, the caller made the frame with
# call ...,[d2,d3,a2,a3],212). d0, d1, a1 are written there before they are read; d2..a3 are saved by that call. drv_edp_draw
# (editor.c) draws our editor and returns 1: then return as the renderer does; 0 - the native code (the 7 displaced bytes).
# Code of the core image (loaded past the cache) and the uncached alias only: no core variable is touched on CPU 1
_drv_tramp_c3d:
	mov	_drv_edp_shared+0x40000000,a1
	mov	(4,a1),d0
	inc	d0
	mov	d0,(4,a1)
	movhu	(0x8050),d0			# the CPU, as drv_cpu reads it
	mov	d0,(8,a1)
	mov	a0,a2
	add	-32,sp
	call	_drv_edp_draw,[],0
	add	32,sp
	cmp	0,d0
	beq	1f
	ret	[d2,d3,a2,a3],212
1:	mov	a2,a0
	mov	0x60f580a8,a1
	jmp	c3d_back

# OpenVG 1.1 of the firmware (editor.c, CPU 1). Direct entries (past the movm/add of the table entry) with
# the registers and frames of the firmware's own call sites; pointers go to a0/a1 first. The addresses are in fw.ld
_drv_vg_create_path:				# (format, type, scale, bias, seg_hint, coord_hint, caps): d0, d1, a0, a1, (4..12,sp)
	add	-16,sp
	mov	(36,sp),a0
	mov	a0,(4,sp)
	mov	(40,sp),a0
	mov	a0,(8,sp)
	mov	(44,sp),a0
	mov	a0,(12,sp)
	mov	(28,sp),a0
	mov	(32,sp),a1
	call	vg_create_path,[d2,d3,a2,a3],36
	add	16,sp
	rets

_drv_vg_clear_path:				# (path, caps)
	add	-4,sp
	call	vg_clear_path,[d2,d3],12
	add	4,sp
	rets

_drv_vg_append:					# (path, nseg, *seg, *data): d0, d1, a0, a1
	mov	(12,sp),a0
	mov	(16,sp),a1
	add	-4,sp
	call	vg_append,[d2,d3,a2,a3],28
	add	4,sp
	rets

_drv_vg_draw_path:				# (path, modes)
	add	-4,sp
	call	vg_draw_path,[d2,d3,a2,a3],32
	add	4,sp
	rets

_drv_vg_destroy_path:
	add	-4,sp
	call	vg_destroy_path,[d2],8
	add	4,sp
	rets

_drv_vg_create_paint:
	add	-4,sp
	call	vg_create_paint,[],4
	add	4,sp
	rets

_drv_vg_set_color:				# (paint, rgba)
	add	-4,sp
	call	vg_set_color,[d2,d3],12
	add	4,sp
	rets

_drv_vg_set_paint:				# (paint, modes)
	add	-4,sp
	call	vg_set_paint,[d2,d3],12
	add	4,sp
	rets

_drv_vg_destroy_paint:
	add	-4,sp
	call	vg_destroy_paint,[d2],8
	add	4,sp
	rets

_drv_vg_seti:					# (type, value)
	add	-4,sp
	call	vg_seti,[d2,d3],12
	add	4,sp
	rets

_drv_vg_setfv:					# (type, count, *values): d0, d1, a0
	mov	(12,sp),a0
	add	-4,sp
	call	vg_setfv,[d2,d3,a2],16
	add	4,sp
	rets

_drv_vg_setf:					# (type, value bits)
	add	-4,sp
	call	vg_setf,[d2,d3],12
	add	4,sp
	rets

_drv_vg_load_identity:
	add	-4,sp
	call	vg_load_identity,[],4
	add	4,sp
	rets

_drv_vg_clear:					# (x, y, w, h): d0, d1, a0, a1
	mov	(12,sp),a0
	mov	(16,sp),a1
	add	-4,sp
	call	vg_clear,[d2,d3,a2,a3],28
	add	4,sp
	rets

_drv_c3d_swap:					# the renderer's eglSwapBuffers of its display and surface (0x625b4744, 0x625b4750)
	add	-4,sp
	call	c3d_swap,[],4
	add	4,sp
	rets

# Loading profiles from outside (import.c). All of it was seen on CPU 0 only (USB tasks, the application task for the
# parse), where the C code and the ID search below may use core variables (cached memory of CPU 0). On another CPU
# they are not touched: the stock path (the check in _drv_tramp_imp_id, drv_cpu() in drv_imp_name and drv_imp_apply).
	.equ	IMP_N, 88
	.global	_drv_tramp_imp_reg, _drv_h970b, _drv_tramp_imp_id, _drv_tramp_imp_name, _drv_tramp_imp_apply, _drv_imp_put

# The vendor operation registration, at mov 0x970c,d0 (0x614ba920): register 0x970B first, as 0x614c0156(d0 code, d1 phase,
# a0 handler, a1 0) is called around here; then the displaced instruction. (0,sp) is free: the firmware calls from here too
_drv_tramp_imp_reg:
	mov	0x970b,d0
	clr	d1
	mov	0,a1
	mov	_drv_h970b,a0
	call	imp_ptp_reg,[d2,d3,a2,a3],20
	mov	0x970c,d0
	jmp	imp_reg_back

# PTP 0x970B GetSupportedMySetFunction, answered with p1 = 3, as by a camera that supports the profile load: a0 the
# request, a1 the response, as the stock handler of 0x9701 (0x614be879). Workspace offers the profile load only with p1 & 3
_drv_h970b:
	movhu	(a0),d0
	movhu	d0,(0xa2645014)
	mov	1,d0
	mov	d0,(12,a1)
	mov	0x2001,d0
	movhu	d0,(a1)
	mov	3,d0
	mov	d0,(16,a1)
	clr	d0
	retf	[],0

# ID -> table B entry 0x6160a694, at its entry (called with call ...,[d2,d3],8 - it ends with retf [d2,d3],8; d0 the ID;
# d1..d3, a0, a1 are written by it first). Called from the value parse of a load (return address 0x6160a850 in mdr): our
# entry for our IDs. Otherwise, and for any other caller (the export: files from the camera keep the stock settings only), the
# displaced instruction and the stock search
_drv_tramp_imp_id:
	movhu	(0x8050),d1			# the CPU, as drv_cpu reads it: our tables are CPU 0's cached data
	cmp	0,d1
	bne	8f
	mov	mdr,d1
	cmp	imp_parse_ret,d1
	bne	8f
	mov	d0,d3
	exthu	d3
	mov	_drv_imp_ids,a1
	mov	_drv_imp_tab,a0
	clr	d2
1:	movhu	(a1),d1
	cmp	d3,d1
	beq	9f
	add	2,a1
	add	16,a0
	inc	d2
	cmp	IMP_N,d2
	blt	1b
8:	mov	0x6160b888,a0
	jmp	imp_id_back
9:	retf	[d2,d3],8

# Name -> ID 0x6160a772, at its end (0x6160a7b7): a2 = 0 found, 1 missed; a3 the name; (4,sp) where the ID goes (u16).
# A miss: our names (drv_imp_name)
_drv_tramp_imp_name:
	cmp	0,a2
	beq	9f
	mov	a3,d0
	add	-32,sp
	call	_drv_imp_name,[],0
	add	32,sp
	cmp	0,d0
	beq	9f
	mov	(4,sp),a0
	movhu	d0,(a0)
	mov	0,a2
9:	mov	a2,d0
	exthu	d0
	ret	[d2,d3,a2,a3],24

# A parsed line before it is applied (0x6160a087 in the load 0x61609ecb): (86,sp) the ID, (80,sp) the value, (72,sp) the
# slot. drv_imp_apply(id, value, slot) = 1: done, to the parse loop's tail 0x6160a135; 0: the displaced compare, stock
_drv_tramp_imp_apply:
	movhu	(86,sp),d0
	mov	(80,sp),d1
	mov	(72,sp),a0
	add	-32,sp
	mov	a0,(12,sp)
	call	_drv_imp_apply,[],0
	add	32,sp
	cmp	0,d0
	beq	1f
	jmp	imp_line_done
1:	mov	(72,sp),d0
	cmp	256,d0
	jmp	imp_apply_back

# (id, value): a Current line written as the loader writes it (0x6160a0ae..0x6160a0c3): the snapshot, the working array
_drv_imp_put:
	mov	d0,(4,sp)
	mov	d1,(8,sp)
	add	-4,sp
	call	imp_snap_put,[d2],4
	mov	(8,sp),d0
	mov	(12,sp),d1
	call	imp_work_put,[d2],4
	add	4,sp
	rets

# ISP colour table override (ispdrv.c). Any CPU, the firmware's job task; the C code
# touches only the arena block (uncached), the job's S/ctx/B and the stack. a0, a1, d0, d1 are saved around it (the
# firmware's code at both sites reads them next); d2, d3, a2, a3 the C code keeps.
	.global	_drv_tramp_isp_w, _drv_tramp_isp_b

# H2, the register writer's entry 0x6037c519 (frame already made by its caller): a0 register base, a1 = S, a3 = ctx,
# d3 the job's register mask. drv_isp_writer(base, S, ctx, mask), then the displaced mov a1,d1; mov a0,d3; mov d3,(sp)
_drv_tramp_isp_w:
	movm	[d2,d3,a2,a3,other],(sp)	# 48 bytes: also d0, d1, a0, a1 and mdr, lir, lar (the writer returns by retf: mdr)
	add	-20,sp
	mov	a3,(12,sp)			# arguments 3 and 4: ctx, mask
	mov	d3,(16,sp)
	mov	a0,d0
	mov	a1,d1
	call	_drv_isp_writer,[],0
	add	20,sp
	movm	(sp),[d2,d3,a2,a3,other]
	mov	a1,d1
	mov	a0,d3
	mov	d3,(sp)
	jmp	isp_w_back

# H1, the colour builder's entry 0x603e8fba: a0 = ctx. drv_isp_builder(ctx), then the displaced mov a0,d3; mov d3,a3;
# inc4 a3
_drv_tramp_isp_b:
	movm	[d2,d3,a2,a3,other],(sp)
	add	-12,sp
	mov	a0,d0
	call	_drv_isp_builder,[],0
	add	12,sp
	movm	(sp),[d2,d3,a2,a3,other]
	mov	a0,d3
	mov	d3,a3
	inc4	a3
	jmp	isp_b_back

# The look's grain (graindrv.c), S and A. Any CPU, the firmware's job task; the C code touches only the arena block
# (uncached), the job's ctx, request B and register image, and the stack.
	.global	_drv_tramp_grain_sel, _drv_tramp_grain_set

# S, the blend-case selector's entry 0x603ecda1: a0 = ctx; its callers made the frame (call ...,[d2,a2],12) and every exit
# is ret [d2,a2],12 with the case in d0. The selector only reads and keeps d3, a3: it is called here the way its callers
# call it (grain_sel_stock: the displaced instructions, then the rest of it), then drv_grain_sel(ctx, stock case) gives
# the case to return. Our frame: (0,sp) the slot call writes its return address to, (4,sp) ctx
_drv_tramp_grain_sel:
	add	-8,sp
	mov	a0,(4,sp)
	call	grain_sel_stock,[d2,a2],12
	mov	d0,d1
	mov	(4,sp),d0
	add	-12,sp
	call	_drv_grain_sel,[],0
	add	12,sp
	add	8,sp
	ret	[d2,a2],12
grain_sel_stock:
	mov	a0,a2				# displaced: mov a0,a2; inc4 a2; mov (a2),a1; movbu (16,a1),d0
	inc4	a2
	mov	(a2),a1
	movbu	(16,a1),d0
	jmp	grain_sel_back

# A, after the blend setup of a pass 0x6036fc74 (in 0x6036fc58, the case is not 0): d2 the case, (a2) = ctx.
# drv_grain_set(ctx, case) with every register kept, then the displaced mov (a2),a0; mov (24,a0),a0
_drv_tramp_grain_set:
	movm	[d2,d3,a2,a3,other],(sp)
	add	-12,sp
	mov	(a2),d0
	mov	d2,d1
	call	_drv_grain_set,[],0
	add	12,sp
	movm	(sp),[d2,d3,a2,a3,other]
	mov	(a2),a0
	mov	(24,a0),a0
	jmp	grain_set_back

# The look's grain (graindrv.c), its texture. W and H3 run in the firmware's job creator and deleter tasks, H2 in the
# creator task inside the texture builder; the C code keeps to the arena blocks, the job and the stack, and calls the firmware only through
# the thunks below (the calls the stock makes at the same places). Every register is kept: movm with other is 48
# bytes - d2, d3, a2, a3, the seven 'other' registers and a dummy word the group adds below them (MN10300 MOVM:
# 4 * (n + 1) bytes with other). H2 reads the builder's frame above them; at +44 it read the wrong word.
	.global	_drv_tramp_grain_h2, _drv_tramp_grain_h3
	.global	_drv_fw_tex_build, _drv_fw_tex_release

# H2, in the tile node builder 0x603a5167 at 0x603a5314: its sp holds the record (44,sp) and the tile descriptor (72,sp).
# drv_grain_tex_fill(the builder's sp), then the displaced mov sp,a0; add 164,a0
_drv_tramp_grain_h2:
	movm	[d2,d3,a2,a3,other],(sp)
	mov	sp,a0
	add	48,a0				# the builder's sp, above what movm saved (48 bytes, see below)
	mov	a0,d0
	add	-12,sp
	call	_drv_grain_tex_fill,[],0
	add	12,sp
	movm	(sp),[d2,d3,a2,a3,other]
	mov	sp,a0
	add	164,a0
	jmp	grain_h2_back

# H3, the cleanup gate 0x6038a5bf at 0x6038a5d0 (P+8 is 0 or 2): a0 = h. drv_grain_tex_clean(h), then the displaced
# mov (11308,a0),d0; cmp 12,d0 - its flags are what the bne at the return address tests
_drv_tramp_grain_h3:
	movm	[d2,d3,a2,a3,other],(sp)
	add	-12,sp
	mov	a0,d0
	call	_drv_grain_tex_clean,[],0
	add	12,sp
	movm	(sp),[d2,d3,a2,a3,other]
	mov	(11308,a0),d0
	cmp	12,d0
	jmp	grain_h3_back

# W, the planner selector 0x6038a4f0 at 0x6038a531 (P+8 is 0 or 2; a2 = h, a3 = P): drv_grain_wl(h) 0 - the stock
# key test (the displaced mov (11308,a2),d0; cmp 12,d0, whose flags the bne at the return address tests); 1 - ours: the
# key h+11308 is the grain film's now, the ART planner 0x6038c072 is called as 0x6038a53a calls it, then
# drv_grain_wl_after(h) puts the job's key back, and the selector goes on at 0x6038a549 with the planner's result in d0.
# A word above the saved registers keeps the result across the C call
	.global	_drv_tramp_grain_wl
_drv_tramp_grain_wl:
	add	-4,sp
	movm	[d2,d3,a2,a3,other],(sp)
	add	-12,sp
	mov	a2,d0
	call	_drv_grain_wl,[],0
	add	12,sp
	mov	d0,(48,sp)			# the slot above the 48 bytes movm saved
	movm	(sp),[d2,d3,a2,a3,other]
	mov	(sp),d0
	cmp	0,d0
	bne	1f
	add	4,sp
	mov	(11308,a2),d0
	cmp	12,d0
	jmp	grain_wl_back
1:
	mov	a2,a0
	add	-4,sp
	call	fw_art_plan,[],4
	add	4,sp
	mov	d0,(sp)
	movm	[d2,d3,a2,a3,other],(sp)
	add	-12,sp
	mov	a2,d0
	call	_drv_grain_wl_after,[],0
	add	12,sp
	movm	(sp),[d2,d3,a2,a3,other]
	mov	(sp),d0
	add	4,sp
	jmp	grain_wl_after

# D, the pass setup's check 0x6036fbbb (its caller 0x6036f7a1 made the frame: call ...,[d2],4; a0 = ctx; the answer in
# d0, retf [d2],4): drv_grain_detail(ctx) 0 - the stock check (the displaced mov (24,a0),a1; mov 1,d2, then the rest of
# it); 1 + the answer - returned as the check returns (movm restored MDR, the return address). A word above the saved
# registers keeps the C result
	.global	_drv_tramp_grain_det
_drv_tramp_grain_det:
	add	-4,sp
	movm	[d2,d3,a2,a3,other],(sp)
	add	-12,sp
	mov	a0,d0
	call	_drv_grain_detail,[],0
	add	12,sp
	mov	d0,(48,sp)
	movm	(sp),[d2,d3,a2,a3,other]
	mov	(sp),d0
	add	4,sp
	cmp	0,d0
	bne	1f
	mov	(24,a0),a1
	mov	1,d2
	jmp	grain_det_back
1:
	add	-1,d0
	retf	[d2],4

# The texture builder 0x603a3bdb as 0x6038e6a6 calls it: a0 = P (h+4), a1 = the 6 parameter bytes -> a0 the record or 0.
# d0 P, d1 parameters -> d0
_drv_fw_tex_build:
	mov	d0,a0
	mov	d1,a1
	add	-4,sp
	call	fw_tex_build,[d2,d3,a2,a3],28
	add	4,sp
	mov	a0,d0
	rets

# The texture release 0x603a452a as 0x6038e8c0 calls it: a0 = P, a1 = the record. d0 P, d1 record
_drv_fw_tex_release:
	mov	d0,a0
	mov	d1,a1
	add	-4,sp
	call	fw_tex_release,[a2,a3],12
	add	4,sp
	rets

# ------------------------------------------------------------------------------------------------------------------
# The look's optics rows (pictprof.c, "The look's optics rows"): items ROW_I0 (Bloom) and ROW_I1 (Halation), in 7..5377 (the
# range of the column's switches); their children are sliders of kind 0 on the virtual settings ROW_SET + row, values
# 0..ROW_N - 1. The numbers are pictprof.c's (ITEM_BLOOM, ITEM_HALATION, ROW_SET, ROW_STR, OMK_OPTICS_MAX + 1). GUI task, CPU 0
	.equ	ROW_I0, 5372
	.equ	ROW_I1, 5370
	.equ	ROW_SET, 0x7e00
	.equ	ROW_SEND, 0x7e02
	.equ	ROW_N, 6
	.equ	ROW_STR, 0x7f00
	.global	_drv_row_find1, _drv_row_find2, _drv_row_fac0, _drv_row_fac1, _drv_row_bar, _drv_row_hold
	.global	_drv_row_stop, _drv_row_order, _drv_row_sign, _drv_row_digit, _drv_row_str
	.global	_drv_ui_count, _drv_ui_byidx, _drv_ui_selectable

# The redraw's item table search, 0x60e49d6b (mov d3,a0; movhu (a0),d2; mov 0x611b50f8,a0): our descriptors first
_drv_row_find1:
	mov	d3,a0
	movhu	(a0),d2
	mov	_drv_row_desc,a0
	movhu	(a0),d0
	cmp	d2,d0
	beq	1f
	add	40,a0
	movhu	(a0),d0
	cmp	d2,d0
	beq	1f
	mov	0x611b50f8,a0
	jmp	find1_back
1:	mov	a0,(4452,a3)
	jmp	find1_found

# The left column's search, 0x60e4a1a2 (exthu d3; clr d1; mov 0x611b50f8,a3): the same, the descriptor to (16,sp)
_drv_row_find2:
	exthu	d3
	clr	d1
	mov	_drv_row_desc,a3
	movhu	(a3),d0
	cmp	d3,d0
	beq	1f
	add	40,a3
	movhu	(a3),d0
	cmp	d3,d0
	beq	1f
	mov	0x611b50f8,a3
	jmp	find2_back
1:	mov	a3,(16,sp)
	jmp	find2_found

# The child factory's cases of our rows (switch 0x611a0a98): a kind-0 child on the virtual setting
_drv_row_fac0:
	mov	ROW_SET,d0
	jmp	fac_kind0
_drv_row_fac1:
	mov	ROW_SET+1,d0
	jmp	fac_kind0

# The redraw's case of our rows (switch 0x611b79a8): the stock slider handler (it finds no value for an unknown item),
# then drv_row_draw(panel) puts the row's value on the bar
_drv_row_bar:
	mov	a3,a0
	mov	a0,a1
	call	item_slider_fw,[d2,d3,a2,a3],40
	mov	a3,d0
	add	-32,sp
	call	_drv_row_draw,[],0
	add	32,sp
	jmp	item_back

# The column's long OK (0x903e), 0x60e3ca64 (exthu d0; cmp 5029,d0), d0 the item under the cursor, a3 the column + 72:
# our rows are reset here (drv_row_reset(item) -> 1, the column's result in d2), then the row's child reads its value
_drv_row_hold:
	exthu	d0
	cmp	ROW_I0,d0
	beq	1f
	cmp	ROW_I1,d0
	beq	1f
	cmp	5029,d0
	jmp	hold_back
1:	add	-32,sp
	call	_drv_row_reset,[],0
	add	32,sp
	mov	d0,d2
	mov	(4,a3),a0			# the row's child (as 0x60e3ca7f passes the editors' long OK to it)
	cmp	0,a0
	beq	2f
	call	child_update,[d2,d3,a2,a3],68	# it reads its value again (else its next step starts from the old one)
2:	jmp	hold_none

# The slider child's setup (0x60e2c3..): +116 = 1 (stop at the ends, no wrap) for the ids of the table
# 0x611979ac (239 ids), set at 0x60e2c46a (clr d0; mov 116,a0; add a3,a0) before its search: ours get 1
_drv_row_stop:
	mov	(16,sp),a1			# the child's setting id (the stock code loads a1 again right after)
	movhu	(a1),d0
	cmp	ROW_SET,d0
	bcs	1f
	cmp	ROW_SEND,d0
	bcc	1f
	mov	1,d0
	bra	2f
1:	clr	d0
2:	mov	116,a0
	add	a3,a0
	jmp	stop_back

# ... its value list's order: a2 = 1 (top-down) for the ids of the table 0x61197b8a (324 ids), searched before 0x60e2c637
# (mov (16,sp),a0; movhu (a0),d0; cmp 469,d0); ours get 1, and the control's +152 = 1 (the dials step one fixed way, as
# 0x60e3159f sets it for the stock sliders after this builder 0x60e2c539, which keeps its a0 at (24,sp))
_drv_row_order:
	mov	(16,sp),a0
	movhu	(a0),d0
	cmp	ROW_SET,d0
	bcs	1f
	cmp	ROW_SEND,d0
	bcc	1f
	mov	1,a2
	mov	(24,sp),a0
	mov	1,d0
	mov	d0,(152,a0)
	mov	(16,sp),a0
	movhu	(a0),d0
1:	cmp	469,d0
	jmp	order_back

# The left cell's readout: the sign (0x60e4b02c) and digit (0x60e4b09d) lookups pick a table by the item; ours take item
# 5376's -5..+5 tables. At 0x60e4b043 / 0x60e4b0b4 (cmp 5376,d0; beq <5376's case>), the beq said again with absolute jumps
_drv_row_sign:
	cmp	5376,d0
	beq	1f
	cmp	ROW_I0,d0
	beq	1f
	cmp	ROW_I1,d0
	beq	1f
	jmp	sign_back
1:	jmp	sign_11
_drv_row_digit:
	cmp	5376,d0
	beq	1f
	cmp	ROW_I0,d0
	beq	1f
	cmp	ROW_I1,d0
	beq	1f
	jmp	digit_back
1:	jmp	digit_11

# The string getter 0x60d52ed5 (call ..,[d2],8; d2 the string id -> a0 UTF-16), at 0x60d52edb (extbu d0; asl2 d0;
# mov 0x61e53d30,a0): our ids ROW_STR + row are our titles
_drv_row_str:
	mov	d2,a0
	cmp	ROW_STR,a0
	beq	1f
	cmp	ROW_STR+1,a0
	beq	2f
	extbu	d0
	asl2	d0
	mov	0x61e53d30,a0
	jmp	str_back
1:	mov	_drv_row_text,a0
	ret	[d2],8
2:	mov	_drv_row_text+24,a0
	ret	[d2],8

# UI object +24 / +28 / +36 for the virtual settings (method words); any other id - the firmware's method
_drv_ui_count:
	cmp	ROW_SET,d0
	bcs	9f
	cmp	ROW_SEND,d0
	bcc	9f
	mov	ROW_N,d0
	rets
9:	jmp	orig_ui_count
_drv_ui_byidx:
	cmp	ROW_SET,d0
	bcs	9f
	cmp	ROW_SEND,d0
	bcc	9f
	mov	d1,d0
	cmp	ROW_N,d0
	bcs	2f
	mov	-1,d0
2:	cmp	0,a1
	beq	3f
	mov	d0,(a1)
3:	rets
9:	jmp	orig_ui_byidx
_drv_ui_selectable:
	cmp	ROW_SET,d0
	bcs	9f
	cmp	ROW_SEND,d0
	bcc	9f
	clr	d0
	cmp	ROW_N,d1
	bcc	2f
	mov	1,d0
2:	rets
9:	jmp	orig_ui_selectable

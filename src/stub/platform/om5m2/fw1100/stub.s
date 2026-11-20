# OMKit loader v2. The whole patch to main: the FastEndCb hook and this code.
# omk_hook does not write to memory: writing to the tail of section 2 on the camera causes a crash. It only creates a task, stack from the kernel pool. Frame 16.
omk_hook:
	mov	OMK_PRIO,d0
	mov	d0,(4,sp)
	mov	OMK_STACK_LEN,d0
	mov	d0,(8,sp)
	clr	d0
	mov	d0,(12,sp)
	mov	2,d0				# start immediately
	clr	d1
	mov	omk_name,a0
	mov	omk_task,a1
	call	f_acre,[d2,d3,a2,a3],56
	ret	[],16

# OMK_LOAD task: waits for the sections and the card, reads CORE.BIN, checks it, transfers control. On any error the task exits.
omk_task:
	mov	OMK_SEC_TRIES,d2
0:	mov	(omk_sections),d0
	and	0x3ff,d0
	cmp	0x3ff,d0
	beq	1f
	mov	OMK_SEC_DELAY,d0
	call	f_sleep,[d2],8
	add	-1,d2
	bne	0b
	bra	8f
1:	mov	(omk_done),d0			# tail of section 9: writable, 0 again at power-on
	cmp	0,d0
	bne	8f
	mov	1,d0
	mov	d0,(omk_done)
	mov	4,d0				# OK held: 0
	call	f_keyup,[d2,d3,a2,a3],24
	cmp	0,d0
	beq	8f
	mov	OMK_TRIES,d2
1:	mov	omk_path,a0
	mov	omk_mode,a1
	call	f_fopen,[],4
	mov	a0,a3
	mov	a3,d0
	cmp	0,d0
	bne	2f
	mov	OMK_DELAY,d0			# no card, no file, or the card is not ready yet
	call	f_sleep,[d2],8
	add	-1,d2
	bne	1b
	bra	8f
2:	mov	OMK_BUF,a0
	mov	1,d0
	mov	OMK_MAX,d1
	mov	a3,a1
	call	f_fread,[d2,d3,a2,a3],24
	mov	d0,d3
	mov	a3,a0
	call	f_fclose,[d2,a2,a3],16
	mov	OMK_BUF,a2
	mov	(a2),d0				# {magic, body length, sum of body words, ABI}
	cmp	OMK_MAGIC,d0
	bne	8f
	mov	(12,a2),d0
	cmp	OMK_ABI,d0
	bne	8f
	mov	(4,a2),d1
	mov	d1,d0
	and	3,d0
	bne	8f
	cmp	0,d1
	beq	8f
	mov	d1,d0
	add	16,d0
	cmp	d3,d0
	bne	8f
	mov	(8,a2),d2
	mov	a2,a0
	add	16,a0
	clr	d0
3:	mov	(a0),d3
	add	d3,d0
	add	4,a0
	add	-4,d1
	bne	3b
	cmp	d2,d0
	bne	8f
	mov	OMK_ENTRY,a0
	calls	(a0)
8:	call	f_ext,[a2],8

omk_name:	.asciz	"OMK_LOAD"
omk_path:	.asciz	"C:\\OMKIT\\CORE.BIN"
omk_mode:	.asciz	"r"

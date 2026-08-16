/* Minimal reproduction of the arch/microblaze/kernel/entry.S defect in
   PR 121432, with no Linux involved.

   C_ENTRY(_interrupt) does:

	addik	r1, r1, -PT_SIZE	; allocate pt_regs
	SAVE_REGS
	swi	r11, r1, PT_R1		; stash the entry SP at pt_regs+4
	...
	addik	r5, r1, 0		; Arg 1 = struct pt_regs *
	rtbd	r0, do_IRQ

   r1 is the pt_regs base at the point of the call, so the ABI argument
   save area the callee owns -- caller_sp+4 upwards -- IS pt_regs.  A
   callee that spills its incoming argument register lands on PT_R1.

   FIX_ARGS_AREA=1 lowers r1 by C_ARG_SIZE across the call so the callee
   gets scratch, and restores it after, mirroring the proposed entry.S fix.  */

#ifndef FIX_ARGS_AREA
#define FIX_ARGS_AREA 0
#endif

/* Big enough to look like pt_regs; only PT_R1 matters here.  */
#define PT_SIZE		128
#define PT_R1		4

/* Link word + MAX_ARGS_IN_REGISTERS * UNITS_PER_WORD = 4 + 24, rounded up.  */
#define C_ARG_SIZE	32

	.text
	.globl	fake_interrupt
	.type	fake_interrupt,@function

/* int fake_interrupt (void); returns the value found at pt_regs+PT_R1
   after calling the C handler.  The caller passes the sentinel in r5.  */
fake_interrupt:
	addik	r1, r1, -PT_SIZE	/* allocate "pt_regs" */
	swi	r15, r1, 64		/* preserve our return address in it */
	swi	r5,  r1, PT_R1		/* SAVE_REGS: stash sentinel at PT_R1 */

#if FIX_ARGS_AREA
	/* Reserve the argument save area the callee is entitled to use. */
	addik	r1, r1, -C_ARG_SIZE
	brlid	r15, handler
	addik	r5, r1, C_ARG_SIZE	/* delay slot: Arg 1 = pt_regs * */
	addik	r1, r1, C_ARG_SIZE	/* drop it again */
#else
	brlid	r15, handler
	addik	r5, r1, 0		/* delay slot: Arg 1 = pt_regs * */
#endif

	lwi	r3, r1, PT_R1		/* did the sentinel survive? */
	lwi	r15, r1, 64
	rtsd	r15, 8
	addik	r1, r1, PT_SIZE		/* delay slot */
	.size	fake_interrupt, .-fake_interrupt

/* Minimal MicroBlaze bare-metal startup for qemu -M petalogix-s3adsp1800.  */
	.text
	.global _start
	.align 2
_start:
	addik	r1, r0, _stack_top	/* stack pointer */
	addik	r1, r1, -32
	brlid	r15, cmain
	nop
halt:
	bri	halt

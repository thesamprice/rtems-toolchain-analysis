	.text
	.align	2
	.globl	spill_incoming_arg
	.ent	spill_incoming_arg
	.type	spill_incoming_arg, @function
spill_incoming_arg:
	.frame	r1,28,r15		# vars= 0, regs= 0, args= 24
	.mask	0x00008000
	addik	r1,r1,-28
	swi	r5,r1,32
	swi	r15,r1,0
	brlid	r15,use
	
	addik	r5,r1,32
	lwi	r15,r1,0
	rtsd	r15,8 
	
	addik	r1,r1,28
	.end	spill_incoming_arg
$Lfe1:
	.size	spill_incoming_arg,$Lfe1-spill_incoming_arg
	.align	2
	.globl	caller_reserves_area
	.ent	caller_reserves_area
	.type	caller_reserves_area, @function
caller_reserves_area:
	.frame	r1,32,r15		# vars= 0, regs= 1, args= 24
	.mask	0x00088000
	addik	r1,r1,-32
	swi	r19,r1,28
	swi	r15,r1,0
	brlid	r15,other
	
	addk	r19,r5,r0
	brlid	r15,callee
	
	addk	r5,r19,r0
	lwi	r15,r1,0
	lwi	r19,r1,28
	rtsd	r15,8 
	
	addik	r1,r1,32
	.end	caller_reserves_area
$Lfe2:
	.size	caller_reserves_area,$Lfe2-caller_reserves_area

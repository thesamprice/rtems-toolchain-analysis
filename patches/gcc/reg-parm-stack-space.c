/* Verify the MicroBlaze argument save area contract.

   REG_PARM_STACK_SPACE is non-zero on this target, so a caller must
   reserve stack space for the arguments it passes in registers, and the
   callee is entitled to spill its incoming register arguments into that
   area -- which lives in the caller's frame, above the callee's own.

   Hand-written assembly that calls into C must therefore reserve the
   area too.  See PR 121432, where a Linux interrupt handler did not and
   a callee's spill landed inside its pt_regs.  */

/* { dg-do compile } */
/* { dg-options "-O2" } */

extern void use (int *p);
extern void callee (int a);
extern int other (void);

/* A callee may spill its incoming argument register into the area its
   caller reserved.  */
void
spill_incoming_arg (int a)
{
  use (&a);
}

/* A caller reserves that area, even with no locals of its own.  */
void
caller_reserves_area (int a)
{
  other ();
  callee (a);
}

/* MAX_ARGS_IN_REGISTERS * UNITS_PER_WORD = 6 * 4.  */
/* { dg-final { scan-assembler-times "args= 24" 2 } } */

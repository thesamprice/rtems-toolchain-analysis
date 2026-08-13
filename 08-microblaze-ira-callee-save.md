# The MicroBlaze "GCC 15 regression" is an ABI violation in Linux, not a compiler bug

GCC 15 broke the MicroBlaze Linux kernel. Buildroot carries a GCC patch to work around it, and
[PR 121432](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=121432) has been open since August 2025
arguing about whose bug it is.

**It is not a compiler bug.** The MicroBlaze ABI requires the *caller* to reserve space for the
callee's incoming register arguments, and permits the callee to spill them there.
`arch/microblaze/kernel/entry.S` does not reserve it before calling into C. GCC has always been
allowed to emit that store; GCC 15 merely started doing it more often.

The reproducer is four lines of C and it fails the same way on **GCC 12**. The defect and the fix
are both demonstrated end to end under QEMU in [`repro/microblaze-entry/`](repro/microblaze-entry/),
bare-metal, with no Linux toolchain and no kernel build (§6).

---

## 1. What happened

| | |
|---|---|
| `3b9b8d6cfdf5` (GCC 15) | IRA scales callee-save cost by entry-block frequency |
| effect | callee-saved registers get expensive; IRA prefers caller-save + spill |
| symptom | MicroBlaze Linux boots to `Run /init as init process`, then hangs |
| bisected to | that commit, by Buildroot |
| Buildroot's response | [a GCC patch](evidence/microblaze-ira/buildroot-0002-gcc-config-microblaze-fix-ira-for-GCC15.patch) adding `TARGET_CALLEE_SAVE_COST` returning constant `1` |

The IRA change itself is three lines in `gcc/ira-color.cc`:

```c
 	    add_cost = ((ira_memory_move_cost[mode][rclass][0]
 		         + ira_memory_move_cost[mode][rclass][1])
 		        * saved_nregs / hard_regno_nregs (hard_regno,
-							  mode) - 1);
+							  mode) - 1)
+		       * (optimize_size ? 1 :
+			  REG_FREQ_FROM_BB (ENTRY_BLOCK_PTR_FOR_FN (cfun)));
```

It was written for PR 111673: without the scaling, IRA would pick a callee-save register even
when caller-save registers were free, defeating shrink-wrapping.

## 2. The ABI contract

`gcc/config/microblaze/microblaze.cc:2100-2117` documents the frame:

```
             Before call		        After call
        +-----------------------+	+-----------------------+
   high |  local variables,     |	|  local variables,	|
   mem. |  callee saved and     |       |  callee saved and    	|
	|  temps     		|       |  temps     	        |
        +-----------------------+	+-----------------------+
        |  arguments for called	|       |  arguments for called |
	|  subroutines		|	|  subroutines  	|
        |  (optional)           |       |  (optional)           |
        +-----------------------+	+-----------------------+
	|  Link register 	|	|  Link register        |
    SP->|                       |       |                       |
```

"arguments for called subroutines" sits in the **caller's** frame, above the link register slot.
That is the area a callee may use to spill the arguments it received in `r5`–`r10`.

This is not a reading of prose. Every caller GCC emits reserves it —
[`caller-frame.c`](evidence/microblaze-ira/caller-frame.c):

```
caller:
	.frame	r1,32,r15		# vars= 0, regs= 1, args= 24
```

**`args= 24`** — 24 bytes, six argument words, in a function whose own locals are empty.

## 3. The reproducer

[`spill-incoming-arg.c`](evidence/microblaze-ira/spill-incoming-arg.c), in full:

```c
extern void use (int *p);
void spill_arg (int a) { use (&a); }
```

Taking the address of a register parameter forces it to memory. Where does it go?

```
spill_arg:
	.frame	r1,28,r15		# vars= 0, regs= 0, args= 24
	addik	r1,r1,-28
	swi	r5,r1,32        <-- r1+32, four bytes ABOVE this 28-byte frame
```

`r1+32` is `caller_sp + 4` — the first argument slot in the caller's frame. Compare the kernel's
`do_IRQ` as built by GCC 15 (PR 121432 comment #11):

```
+   0:  3021ffe0        addik   r1, r1, -32
+   c:  f8a10024        swi     r5, r1, 36     <-- r1+36 = caller_sp + 4
```

Same construct. **The reproducer above was compiled with GCC 12.4.1**, three major versions before
the "regression". This codegen is not new and not a GCC 15 behaviour.

## 4. Where the kernel breaks

Gopi Kumar Bulusu identified this in PR 121432 comments #19 and #27. `C_ENTRY(_interrupt)` does:

```asm
        addik   r1, r1, -PT_SIZE
        SAVE_REGS
        ...
        swi     r11, r1, PT_R1
```

It allocates `PT_SIZE` for `pt_regs` and nothing else. There is no argument save area between
`pt_regs` and the C function it then calls, so `do_IRQ`'s `swi r5, r1, 36` lands inside
`pt_regs` — on `PT_R1`, the saved stack pointer.

GCC 14 assigned `r22`, a callee-saved register, and never needed the spill slot. The kernel worked
by luck. GCC 15's costing removed the luck.

## 5. Why the Buildroot patch should not be upstreamed

The patch adds:

```c
static int
microblaze_callee_save_cost (spill_cost_type, unsigned int hard_regno, machine_mode,
		       unsigned int, int mem_cost, const HARD_REG_SET &, bool)
{
  return 1;
}
```

Four objections, in increasing order of seriousness:

**It ignores every parameter.** `hard_regno`, `mode` and `mem_cost` are all unused. It is not a
cost function; it is a constant that happens to restore pre-GCC-15 behaviour.

**The cited model does not do this.** The patch points at i386 as precedent. `i386.cc:21231-21245`
returns `1` only when `mem_cost <= 2` or when optimising for size, and `mem_cost - 2` otherwise,
with a comment explaining push/pop encoding sizes. It is a real cost function.

**It disables a correctness-adjacent optimisation globally.** Every MicroBlaze compilation loses
the shrink-wrapping behaviour `3b9b8d6cfdf5` exists to enable, so that one project's hand-written
assembly keeps working.

**Its own author says it is wrong.** PR 121432 comment #10, Romain Naour: *"This is probably not
the correct fix... I hope it help."*

**And it does not actually fix the kernel.** Comment #25 reports the same failure class in
`signal.c:do_notify_resume`; comment #45 reports the system still stalling with the Buildroot
patch applied across five kernel versions.

## 6. Demonstrating it, and the fix, without Linux

The defect is in assembly, so it can be reproduced in assembly. No kernel, no Buildroot, no
container — [`repro/microblaze-entry/`](repro/microblaze-entry/) is a bare-metal MicroBlaze
program that runs under `qemu-system-microblaze` in about two minutes.

`entry_repro.S` mirrors `C_ENTRY(_interrupt)`: allocate `PT_SIZE`, stash a sentinel at `PT_R1`,
call a C handler with `r5 = r1`. The handler is a `do_IRQ` stand-in whose incoming argument must
go to memory, and GCC gives it exactly the store from comment #11:

```
handler:
	.frame	r1,28,r15		# vars= 0, regs= 0, args= 24
	addik	r1,r1,-28
	swi	r5,r1,32        <-- caller_sp + 4
```

`FIX_ARGS_AREA=1` lowers `r1` by `C_ARG_SIZE` across the call and restores it after:

| | `PT_R1` after the call | |
|---|---|---|
| `entry.S` as it is today | `0x90010160` | **CORRUPTED** |
| argument save area reserved | `0xfeedface` | **INTACT** |

The corrupt value is a stack address — it is `&regs`, spilled by the callee onto `PT_R1`.

This is the whole bug and the whole fix, on a host with no Linux toolchain at all, using the
GCC 12.4.1 already in the RTEMS toolchain.

## 7. The real fix

`arch/microblaze/kernel/entry.S` must reserve the argument save area before calling C. That is a
Linux patch, not a GCC one:
[`patches/linux/0001-microblaze-reserve-the-ABI-argument-save-area.patch`](patches/linux/).

Seven call sites pass arguments while r1 is the pt_regs base, and two return conventions are in
use which need different treatment:

| convention | sites | restore goes |
|---|---|---|
| `r15` is the branch itself | `do_IRQ` | after the delay slot |
| `r15` is `label - 8` | `_unaligned_data_exception` | through a trampoline -- `ret_from_exc` is reached from three other sites which make no adjustment |
| two callers converge | `microblaze_kgdb_break`, `sw_exception` | once, at `dbtrap_call+8` |

The middle row is the trap: a restore placed after the delay slot there is never executed, and
the first draft of this patch had exactly that bug. Each of the three shapes was reproduced
bare-metal under QEMU before the patch was written.

**It has now been built and booted, and it is not sufficient.**

Buildroot, GCC 15, Linux 6.18.7, `qemu_microblazeel_mmu_defconfig`, with Buildroot's workaround
patch removed:

| | console |
|---|---|
| unpatched | silent hang after `Run /init as init process` |
| entry.S patched | `Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b` |

`0x0b` is SIGSEGV. The patch demonstrably changes behaviour -- init now starts and faults rather
than hanging -- so the mechanism is real. It is not a fix.

A second audit pass found three call sites the first one missed, because they use `rted` rather
than `bralid`/`brlid`/`rtbd`: `full_exception` and two `do_page_fault` calls. `do_page_fault`
runs on every demand-paging fault, so it looked like the obvious cause of a segfaulting init.
Adding all three changed nothing: the panic is identical before and after.

This is exactly where Gopi Kumar Bulusu arrived in comment #25 after his own entry.S and irq.c
attempt -- the same panic, the same exit code -- and his reading still stands: there are further
sites outside entry.S. Ten call sites there are now covered and it is still not enough, so either
the remaining sites are in other files, or the argument save area is not the whole story.

**Do not treat this patch as a fix.** It is a measured, reproducible step that narrows the
problem.

It is not a one-liner: comment #25 found `do_notify_resume` affected too, and comment #45 shows
the current attempt causing an illegal-opcode exception in kernel mode. Every assembly site that
calls into C needs auditing.

If something *is* wanted in GCC, the defensible change is a genuine `TARGET_CALLEE_SAVE_COST`
using `mem_cost` and `hard_regno`, justified by MicroBlaze's real spill costs — a separate patch
that does not claim to fix this bug.

## 8. How this was identified, and how to debug the next one

The whole question — *is the compiler wrong?* — turns on one thing: **what does the ABI say about
who owns the memory being written?** Everything else is noise. The method:

**Read the store, not the diff.** The assembly diff in comment #11 looks like a register allocation
difference, and that framing sent the discussion toward IRA. The signal is in a single
instruction: `swi r5, r1, 36` against `addik r1, r1, -32`. An offset larger than the frame size is
a write *outside* the frame. That is either an ABI-sanctioned access to the caller's frame, or a
bug — and which one is a documented fact, not a matter of opinion.

**Ask the backend, not the manual.** `microblaze.cc:2100` carries the frame diagram. The compiler's
own source is the authority on what the compiler believes the ABI is, and it is what a maintainer
will be persuaded by.

**Then confirm it empirically, in both directions.** Two four-line files settle it:
`caller-frame.c` shows callers reserving the area (`args= 24`), `spill-incoming-arg.c` shows
callees using it. Neither needs the failing configuration, a kernel, or QEMU.

**Reproduce on the oldest compiler you have.** This was the step that decided it. The bug is framed
as a GCC 15 regression, so the instinct is to reach for GCC 15. Running the reproducer on GCC 12
showed the same store — which proves the codegen is not new, so GCC 15 cannot be where the defect
was introduced. A "regression" that reproduces three versions earlier is an exposure, not a
regression. Sam James said as much in comment #1 — *"That commit means it was latent"* — before
anyone had the mechanism.

**Distrust a fix that restores old behaviour.** `return 1` does not encode a fact about MicroBlaze.
A fix that makes a symptom disappear by reverting a cost model, without explaining what the correct
cost is, is a workaround. That the workaround does not fully work (comment #45) is the expected
outcome, not a surprise.

Time to the conclusion was about fifteen minutes, none of it spent building anything. The failing
configuration — Linux, QEMU, GCC 15 — is where the bug *shows up*, and it is the most expensive
place to study it.

## 9. The GCC-side deliverable: a test, not a fix

There is no GCC bug to fix, but there is a GCC gap: nothing in the testsuite states this contract,
which is why PR 121432 spent fifty comments unable to settle it. Comment #26 asks *"Are you
absolutely sure that there is an issue in the assembly code?"* and the thread never answers.

[`patches/gcc/reg-parm-stack-space.c`](patches/gcc/reg-parm-stack-space.c) is a
`gcc.target/microblaze` test asserting both halves of the contract, and it is independently
upstreamable — it documents intended behaviour and does not depend on who fixes the kernel.

The assertion is on `REG_PARM_STACK_SPACE`, expressed through the `.frame` directive:

```
/* { dg-final { scan-assembler-times "args= 24" 2 } } */
```

`24` is `MAX_ARGS_IN_REGISTERS * UNITS_PER_WORD` (`microblaze.h:441`, `:454`). Verified against
GCC 12.4.1: exactly two matches.

The store offset itself is deliberately *not* asserted, because it is not stable — at `-O0` the
frame pointer is `r19` and the spill is `swi r5,r19,36`; at `-O1`/`-O2`/`-Os` it is
`swi r5,r1,32`. `args= 24` holds at every level.

`REG_PARM_STACK_SPACE` being non-zero is the formal statement of the contract, and is stronger
evidence than the frame diagram in §2: it is what tells the middle end that the caller owns, and
must allocate, that memory.

## 10. What is not established here

- **No GCC 15 build was made.** The mechanism is demonstrated on GCC 12; the exact `do_IRQ`
  allocation from comment #11 is taken from the bug report, not reproduced locally.
- **No kernel fix is proposed or tested.** Section 6 says where it belongs, not what it is.
- **The second failure in comment #45** — stalls after `init` on kernels newer than 4.19 with *any*
  compiler, including GCC 13 and 14 — is a separate issue and is untouched by any of this.

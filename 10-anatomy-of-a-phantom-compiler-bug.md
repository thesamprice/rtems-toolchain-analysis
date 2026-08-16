# Anatomy of a phantom compiler bug

*How a "GCC 15 regression" on MicroBlaze turned out to be a 15-year-old kernel bug, why every
tool we pointed at it lied to us at least once, and what finally worked.*

---

In August 2025, Buildroot's maintainers noticed that their MicroBlaze configuration — unchanged —
stopped booting when built with GCC 15. They bisected GCC and landed on a one-line change to the
register allocator. [GCC PR 121432](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=121432) grew to
fifty comments of maintainers and reporters arguing about whose bug it was. Buildroot shipped a
GCC patch that made the symptom go away, its own author noting *"this is probably not the correct
fix."*

It wasn't. The compiler was never wrong. Here is the whole story, including the parts where we
were.

## The contract

The MicroBlaze ABI has an unusual property, stated formally in GCC's backend: the **caller**
reserves stack space for the arguments it passes in registers, and the **callee** may spill its
incoming argument registers into that area.

```c
/* gcc/config/microblaze/microblaze.h */
#define FIRST_PARM_OFFSET(FNDECL)   (UNITS_PER_WORD)
#define REG_PARM_STACK_SPACE(FNDECL) (MAX_ARGS_IN_REGISTERS * UNITS_PER_WORD)
```

So a callee may write to `[caller_sp+4, caller_sp+28)`. Four lines of C prove it — **on GCC 12**,
three major versions before the "regression":

```c
extern void use (int *p);
void spill_arg (int a) { use (&a); }
```
```asm
	addik	r1,r1,-28
	swi	r5,r1,32        ; caller_sp + 4 — above this function's own frame
```

The bisected GCC 15 commit didn't create that entitlement. It made callee-saved registers
expensive, so the allocator started *using* the entitlement. Anything that had silently depended
on callees not spilling was now exposed.

## The suspect

Linux's `arch/microblaze/kernel/entry.S` builds a `pt_regs` on the kernel stack and then calls C
with `r1` still pointing at its base. The callee's spill area — `caller_sp+4` up — **is the saved
register block**. And `asm-offsets.h` says:

```c
#define PT_R1 4    /* the saved user stack pointer */
```

The callee's *first-argument* spill slot is the interrupted program's stack pointer.

We audited `entry.S`, found ten such call sites, patched them, and were wrong twice in
instructive ways — a restore placed after a delay slot that a `label-8` return convention skips,
and three sites missed because they used `rted` while we grepped for `bralid|brlid|brald|rtbd`.

## Everything lies to you at least once

The middle of this investigation is a catalogue of self-inflicted wounds, preserved in the
[experiment log](09-microblaze-handoff.md) because they're the transferable part:

- Our first reproduction — Linux 6.18.7 — **wasn't the reported bug at all**. Buildroot's own
  workaround didn't fix it (comment #45's *second*, compiler-independent issue). Every dynamic
  conclusion drawn from it was void. The control experiment that revealed this ran *last*; it
  should have run first.
- A `-O0` experiment "disproved" the spill theory — because it forced `-O0` on the wrong files.
  The theory was right; the experiment was aimed at sites 1–10 while the killer was site 11.
- `patch --forward` silently skipped a file carrying an earlier revision, so a boot "tested" hunks
  that were never compiled in.
- A leftover workaround patch from a failed experiment sat inert for ten runs, then **reactivated**
  when a `dirclean` re-ran Buildroot's extract step — turning a "pristine rebuild" back into a
  workaround build. A scripted gate (*hook refs must be 0 in the re-extracted source*) caught it
  before it produced data; that was the first setup error caught by machinery instead of
  hindsight.
- `qemu -nographic` inherits stdin and **ate the rest of the shell heredoc driving it**, echoing
  script text into the guest console and silently ending the script at the QEMU line. Three runs
  produced truncated, plausible-looking logs before we spotted it. (`< /dev/null`. Always.)
- QEMU's `-d int` trace was 4.5 MB of nothing: on a software-managed-TLB machine every TLB miss
  is an exception, and the "interrupt storm" at the tail was the panic path's `__udelay` loop —
  post-mortem noise we misread twice before resolving symbols against `System.map`.

The rule that survived all of it: **verify the artifact, not the intention.** A run means nothing
until the thing under test — the binary, not the source, not the makefile — is confirmed to
contain the change.

## The kernel tells you, if you ask

The instrument that cracked it wasn't the emulator. It was making the kernel do what RTEMS does
on a fault: dump the registers. Ten lines in `do_page_fault` (`show_regs()` plus a banner), one
boot of the *validated* failing baseline:

```
BADFAULT: pid=1 comm=init addr=0000003c code=1
 r1=FFFFFF9C ...  rPC=480293BC  msr=000053A2  ear=0000003C  esr=00001012
```

`r1 = 0xFFFFFF9C` is **-100**, and -100 is **`AT_FDCWD`** — the first argument of every `*at`
syscall. Init had been returned to user space with a syscall argument as its stack pointer. You
could not ask for a cleaner fingerprint: the callee spilled arg1 into `caller_sp+4`, which is
`PT_R1`, which `entry.S` then restored as the user SP.

But none of our ten patched sites explained it — until we stopped grepping for call mnemonics and
looked at the syscall dispatch:

```asm
	lwi	r12, r12, sys_call_table
	addi	r15, r0, ret_from_trap-8
	bra	r12                       ; <- every syscall. bare bra. no audit caught it.
```

**Site 11.** The most-executed call site in the file, invisible to every mnemonic grep because
the return address is set up manually and the branch is a plain `bra`. The durable audit rule:
enumerate asm→C calls by *`r15` assignment*, because setting a return address is the one thing
every call must do — the branch can be anything.

A disassembly sweep of the built vmlinux drove it home: **11,239 functions** store through `r1`
into their caller's argument area. Perfectly legal — every C caller reserves it. Only the eleven
asm boundaries didn't.

## The fix, and the failure that proved it

The fix reserves the area across each call — `addik r1, r1, -32` before, restore after, with a
trampoline where the return convention bypasses the call site. Two boots made it causal rather
than correlational:

- With sites 1–10 + the dispatch patched, init stopped dying with `AT_FDCWD` in `r1` — and
  started dying **in the signal path, executing the sigreturn trampoline**. Because
  `sys_rt_sigreturn_wrapper` derives its `pt_regs` pointer from `r1`, and our fix had moved `r1`.
  The failure relocated *exactly where the mechanism said it must*. One more line fixed the
  wrapper.
- Final boot: pristine GCC 15.3, Linux 6.12.81, twelve-edit `entry.S` — **full shell**, DHCP
  lease, login prompt, zero faults. The same compiler that hangs the stock kernel.

| | stock kernel | patched kernel |
|---|---|---|
| GCC 15 + Buildroot's GCC workaround | boots | — |
| **pristine GCC 15** | **hangs at init** | **boots** |

## Coda

- The kernel patch, with the full evidence chain in the commit message, is in
  [`patches/linux/`](patches/linux/) and headed to LKML. `Cc: stable` — GCC 15 will be used to
  build every LTS tree soon enough.
- GCC needs no change. The Buildroot workaround suppresses the allocator behaviour that exposes
  the bug, for every MicroBlaze program, and should be dropped once the kernel fix lands.
- A `gcc.target/microblaze` test pinning the ABI contract — so the next argument about who owns
  `caller_sp+4` can be settled by `make check` — is staged in [`patches/gcc/`](patches/gcc/).
- RTEMS has the same bug class in its interrupt frame (`bsp_interrupt_dispatch` clobbers the
  saved R4 at `-O0` today); the fix is on a branch, one header and two lines of asm.

Total wall-clock from "look at this Buildroot patch" to a booting fix: one long day. Time the
actual root cause took once the reproduction was *valid* and the kernel was *asked directly*:
about ninety minutes. The other hours were spent learning, repeatedly, that an experiment you
haven't verified the setup of is not an experiment — it's a rumor.

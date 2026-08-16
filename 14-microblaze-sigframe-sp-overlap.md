# 14 — Investigation trail: MicroBlaze signal-handler SP overlaps the siginfo

> **SUPERSEDED by [15-microblaze-cancel-hang-conclusion.md](15-microblaze-cancel-hang-conclusion.md).**
> The `SP==&siginfo` overlap documented here is a *real latent bug* but was shown
> NOT to be the primary trigger (reserving an arg-save area did not cure the
> hang). The confirmed root cause is a kernel race in the interrupts-enabled
> signal-return window — see doc 15. Kept for the investigation trail.

**Supersedes the "race" characterization in 13-microblaze-cancellation-race.md.**
That doc correctly established *not our patch / layout-sensitive / MSR preserved*,
but treated the failure as an unlocated race. A non-perturbing QEMU trace located
it exactly.

## One-line

`arch/microblaze/kernel/signal.c:setup_rt_frame()` sets the signal handler's
stack pointer (`regs->r1`) equal to the frame base, and `struct rt_sigframe` has
`struct siginfo info` at **offset 0** — so **the handler runs with `SP == &siginfo`**.
The MicroBlaze ABI lets a callee use the argument-save/linkage area at `[SP+..]`,
which now overlaps the siginfo. The handler's own register spills (and the
non-preservation of a callee-saved register across the `getpid()` syscall it makes)
corrupt `si_pid`, so glibc's `sigcancel_handler` fails its `si->si_pid != __getpid()`
sanity check and **returns without cancelling**. A `pthread_cancel()` of a thread
blocked in `read()` is silently dropped → hang (and, under other register values,
a fault → the intermittent `tst-eintr1` segfault).

## How it was found (non-perturbing QEMU TCG plugin, guest hung the whole time)

Plugin watched user-space cancel-min addresses under `-icount` (deterministic,
no guest instructions added). Symbols recovered from the exact `cancel-min`
carved out of `linux-min.bin`'s initramfs (its `.text` is offset -4 from a
fresh build, so every downstream address was corrected).

Decisive single-run trace (OUTCOME=HANG, identical at shift=0 and shift=1):

```
sigcancel_handler entry: sp(r1)=48800060  si(r6)=48800060  ctx(r7)=488000e0
                                 ^^^^^^^^^^^^^^^^^^^^^ SP == &siginfo

@100022d0 before `lwi r19,r6,12`:  mem[si+12]=0000005f (=95, correct)  r19=4880023c
   lwi loads si_pid=95 into r19
   swi r7,r1,44                     <- r1(=si-32)+44 = si+12: stomps si_pid with ctx ptr
   brlid __getpid                   <- returns r3=0x5f (=95, correct getpid)
@100022e4 at `xor r19,r19,r3`:     mem[si+12]=488000e0 (stomped)  r19=4880023c (NOT 95!)  r3=5f
   xor 4880023c ^ 5f  != 0  -> "si_pid != getpid()" -> EARLY RETURN
sigcancel_handler -> H_return       <- never reaches the pc-check or __syscall_do_cancel
```

Two independent corruptions, both caused by `SP == &siginfo`:

1. **`r19` (callee-saved) is not preserved across `getpid()`**: it held 95 after
   the `lwi`, but reads back the stale `0x4880023c` after the syscall. The syscall
   register save/restore is corrupted because the user arg-save area it interacts
   with overlaps the frame.
2. **`mem[si+12]` is directly stomped** by the handler's `swi r7,r1,44` — with
   `r1 = si-32`, `r1+44 = si+12`.

Either way `si_pid` is wrong at the compare, so the handler bails.

## What it is NOT (all ruled out with evidence)

- **Not our entry.S argument-save patch** — reproduces on stock `entry.S`
  (workaround-GCC baseline that boots).
- **Not MSR[C]/carry loss across signals** — a high-rate layout-independent probe
  (400 signals, 131 in-window hits) showed `MSR[C]` preserved, 0 leaks.
- **Not a lost cancel CAS** — the worker's `cancelhandling` correctly reads
  `0x08` (CANCELED set) in the handler; the atomic worked.
- **Not the frame-PC/cancel-bridge range** — `framepc` is *in* range; the handler
  would have cancelled if it hadn't bailed on the pid check first.

## Why layout-sensitive (per-build), deterministic per-build

`linux-min.bin` hangs on every boot / every `-icount` shift; a fresh rebuild
(`linux-pristine.bin`) works on every boot. The outcome is decided by whether a
given build's stack offsets make the overlapping spill land on the critical
`si_pid` word — which depends on code/stack layout, hence per-build. This is why
rebuilding "fixes" it and why it looked like a race.

## The kernel code

```c
struct rt_sigframe { struct siginfo info; struct ucontext uc; unsigned long tramp[2]; };
...
static int setup_rt_frame(...) {
    frame = get_sigframe(ksig, regs, sizeof(*frame));   // frame = (sp - size) & -8
    ...
    regs->r1 = (unsigned long) frame;          // SP = frame  == &frame->info  (info @ off 0)
    regs->r6 = (unsigned long) &frame->info;   // si == SP
    ...
}
```

The MicroBlaze ABI reserves the linkage/argument-save area at the *top* of the
callee's incoming frame (`[SP+0 ..]`). Placing `SP` on the siginfo means the
handler (and its callees, and the syscall path) can legally write into the
siginfo. Correct behaviour requires a reserved gap between `SP` and the siginfo.

## Candidate fix that was TRIED and did NOT resolve it

Reserving an ABI argument-save area (`unsigned long arg_save[8]` prepended to
`rt_sigframe` so `SP != &info`) was built and tested. Result:

```
linux-sigfix.bin  -icount shift=0 : HANG 4/4
linux-sigfix.bin  wall-clock      : WORK 3/3
```

i.e. **the same wall/icount split as before the fix — it did not cure the hang.**
So the `SP==&siginfo` memory overlap (handler `swi r7,r1,44` stomping `si+12`) is
a *real latent bug* but not the primary trigger of this hang.

## Refined root cause: r19 not preserved across the getpid() syscall

The decisive corruption is in a **register**, not the frame memory: the handler
does `lwi r19, r6, 12` (r19 = si_pid = 95), then `brlid __getpid`; after getpid
returns, `r19` reads back the stale `0x4880023c` instead of 95. r19 is a
MicroBlaze **callee-saved** register, so `getpid()` must preserve it — the kernel
syscall path does not, in the losing timing.

It is purely **timing-sensitive**, which is the signature of a **nested interrupt
during the syscall** disturbing the saved-register state in kernel entry.S:

- `linux-min.bin`      : hangs at every `-icount` shift (auto/0/3/6)
- `linux-pristine.bin` : works at shifts 1/2/4/5/7/8
- `linux-sigfix.bin`   : hangs at shift 0, works wall-clock

`-icount` shift (and, equivalently, inserting nops) moves when the timer IRQ
lands relative to the syscall window, flipping the outcome. This is the same
*family* as the entry.S argument-save/register-preservation bug, present on
stock entry.S.

## Honest status / next step

Solid: the SIGCANCEL handler bails at its `si_pid != getpid()` guard because
si_pid (via r19) is corrupted; timing-sensitive; not our patch / not MSR[C] /
not a lost CAS / not the frame-PC range; `SP==&siginfo` overlap is real but
secondary. Not yet pinned to the instruction: whether the kernel syscall
save/restore or a nested-interrupt path drops r19. Plugin (userspace) tracing
can't localize it further — it perturbs this marginal timing (adding watchpoints
flips shift-0 from hang to work). The right next instrument is **kernel-side**:
a non-perturbing QEMU `-d exec` / entry.S counter watching the user r19 slot in
`pt_regs` across the getpid syscall and any nested IRQ, correlated with
`System.map`.

## Reproduce

`linux-min.bin` + `qplug/canceltrace.c` (watch `sigcancel_handler`, read
`r1/r6`, `mem[si+12]`, `r19`, `r3` through the prologue). Serial shows the worker
blocked in `read()`, `pthread_cancel` returns 0, `pthread_join` never returns.

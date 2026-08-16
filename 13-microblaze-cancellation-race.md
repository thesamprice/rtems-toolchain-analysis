# 13 — A MicroBlaze pthread-cancellation race (the tst-cancel*/tst-eintr1 failures)

## TL;DR

`pthread_cancel()` of a thread blocked in a **restartable syscall with no data
yet transferred** (the classic case: `read()` on an empty pipe) **intermittently
hangs** on MicroBlaze/QEMU — and under marginally different timing corrupts state
enough to **segfault** (this is Neal's *"tst-eintr1 sometimes segfaulting"*).

It is a **code-layout/timing-sensitive race in the kernel's signal-delivery +
syscall-restart path**, not a source-semantics bug. It reproduces on **stock
`entry.S`** (no argument-save-area patch, GCC callee-save workaround), so it is
**not** caused by the MicroBlaze entry.S fix — the fix merely lets the kernel boot
far enough to run the glibc/LTP testsuite, which is where these tests first run at
all.

## The mechanism

glibc cancels a thread that is inside a cancellable syscall via the
`__syscall_cancel_arch` bridge and a `SIGCANCEL` handler. From glibc
`nptl/pthread_cancel.c` (`sigcancel_handler`), verbatim:

> For interruptable syscalls with external side-effects (i.e. partial reads),
> the kernel will set the IP to after `__syscall_cancel_arch_end`, thus disabling
> the cancellation and allowing the process to handle such conditions.

So cancellation fires **iff** the interrupted PC the handler sees lies in
`[__syscall_cancel_arch_start, __syscall_cancel_arch_end)`:

```
__syscall_cancel_arch_start:
    ... load cancelhandling; branch to __syscall_do_cancel if cancelled ...
    brki r14, 8                 ; the syscall            <- pc must land here
__syscall_cancel_arch_end:      ; (one instruction past brki)
```

For a blocking `read()` with **no** bytes transferred, the correct outcome is
`-ERESTARTSYS`: the kernel rewinds the PC to the `brki` (in range) so that, on
`SIGCANCEL`, `cancellation_pc_check()` succeeds and the cancel is delivered.
That rewind is MicroBlaze `handle_restart()` doing `regs->pc -= 4`
(`arch/microblaze/kernel/signal.c`), called from `do_signal()` — but only when
`in_syscall` is set.

When it works, the kernel does everything right (captured live):

```
DOSIG sig=32 in_syscall=1 pc=100067b0 r3=-512
              ^SIGCANCEL   ^yes        ^=end   ^=-ERESTARTSYS
  handle_restart: pc -= 4  ->  0x100067ac (= brki, IN range)  ->  cancel fires
```

When it **hangs**, the frame glibc sees carries a PC **outside** that range, so
`cancellation_pc_check()` fails, no cancel is delivered, and the `SA_RESTART`
loop re-enters `read()` forever. Under slightly different timing the same window
corrupts state enough to fault — the intermittent segfault.

## Why we call it a race: six consistent data points

Every change that shifts timing/layout — a rebuild, or inserting even ~6
instructions anywhere in the signal path — **deterministically flips hang↔work**.
The failing case cannot be observed from inside the guest, because any probe
perturbs the timing enough to make it pass (a genuine observer effect).

| # | Build (all: stock entry.S + GCC callee-save workaround + clean glibc) | Result |
|---|----------------------------------------------------------------------|--------|
| 1 | pristine build A (`linux-min.bin`)                                    | **HANG** (135s, ×3) |
| 2 | pristine build B — *identical source*, rebuilt (`linux-pristine.bin`) | works  |
| 3 | + `write()` in glibc `sigcancel_handler`                              | works  |
| 4 | + `pr_info()` in kernel `do_signal`                                   | works  |
| 5 | + 6 plain memory stores in kernel `do_signal` (no I/O)                | works  |
| 6 | working captures: `in_syscall=1`, `r3=-ERESTARTSYS`, PC rewound to `brki` (in range) | cancel fires |

Rows 1 vs 2 are the clincher: **same source, two builds, opposite outcomes.**
The only difference is code layout (addresses/alignment), which shifts the race
relative to the per-build-deterministic interrupt/scheduling timing.

## Independence from the entry.S patch

- Reproduced on **stock `entry.S`** (no arg-save reservation) — so the entry.S
  argument-save-area fix does not cause it.
- The entry.S bug was **deterministic** (a spilling syscall corrupts the saved SP
  *every* time — it killed `init` 100% of the time). *Intermittent* segfaults are
  the signature of a race, not a deterministic ABI-reservation defect.
- `nanosleep()`-based cancellation — which goes through the *same*
  `do_notify_resume`/`do_signal` path the patch touches — **works**. Only the
  `read()` case, which additionally depends on the restart-rewind landing the PC
  inside glibc's userspace bridge, hangs.

## Scope

- `cancel a thread in nanosleep()`  : PASS
- `cancel a thread in read()`       : HANG (layout/timing dependent)
- `async-cancel in read()`          : HANG (same)
- `syscall restart (nanosleep)`     : PASS

## Where the fix belongs (hypothesis) and the next instrument

The kernel-visible state at `do_signal` is *correct* in every case we could
observe (`in_syscall=1`, `-ERESTARTSYS`, rewind to `brki`). The race is therefore
in the narrow window between that decision and the PC the delivered signal frame
actually carries — i.e. in the MicroBlaze `do_signal` → `handle_restart` →
`setup_rt_frame` sequence interacting with a preempting hardware interrupt.

The exact racing instruction pair cannot be pinned by in-guest instrumentation
(observer effect, proven above). The only non-perturbing next step is an
**external, deterministic trace**: run the *pristine hanging* image under QEMU
with `-icount shift=auto` (deterministic timing) plus an execution trace or a
plugin watching the guest PC written into the `rt_sigframe`, correlated against
`System.map` for `do_signal`/`handle_restart`/`setup_rt_frame`. Because QEMU-side
tracing adds no guest instructions, it can observe the failing case without
flipping it. This is a substantial separate effort and is the recommended
follow-up for a kernel-maintainer-grade patch (Michal Simek / linux-microblaze).

## Reproducers (in `repro/microblaze-entry/`, scratch mirror `$SP/mbstress/`)

- `cancel-min.c`   — minimal: cancel a thread blocked in `read()`; prints
  `MIN-RC=` only if it does *not* hang.
- `cancel-diag.c`  — isolates cancel/EINTR/restart mechanisms, each in a child
  with a hard timeout so one hang doesn't mask the rest.
- `pc-probe.c`     — reports the interrupted PC vs the cancel-bridge labels.

Canonical hanging image: a pristine build where layout happens to land on the
losing side (build A above). Because the outcome is layout-dependent, "does it
hang" must be treated per-build, not per-source.

## Meta-lessons

- **An observer effect is data.** When every probe makes the bug vanish, stop
  adding probes — the vanishing *is* the proof of a race, and the perturbation
  size bounds the race window (here: ~6 instructions).
- **Rebuild the "same" thing before trusting a flaky result.** Two builds of
  identical source disagreeing is the fastest possible proof of a layout/timing
  race — and it costs one rebuild.
- **Run the control first.** Confirming the hang on *stock* entry.S up front
  would have separated "is it the patch?" from "is it the platform?" immediately.

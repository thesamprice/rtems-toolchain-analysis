# 15 — MicroBlaze read()-cancellation hang: consolidated root cause

**Consolidates docs 13 and 14.** 13 established "not our patch / timing race";
14 chased an SP==&siginfo overlap that turned out to be secondary. This doc is
the current, corroborated conclusion.

## Conclusion (high confidence on the *where*, not yet the exact instruction)

The hang is a **timing-sensitive kernel race in the interrupts-enabled window of
the MicroBlaze syscall-return signal-delivery path** — `do_notify_resume` →
`setup_rt_frame` — disturbed by a **nested timer IRQ and/or a TLB/page-fault
exception** taken *during* signal setup. The disturbance corrupts the context
handed to the signal handler (wrong `pt_regs`/`si_pid`), or re-drives the
`do_notify_resume` loop non-idempotently, so glibc's `sigcancel_handler` fails
its `si_pid == getpid()` guard and drops the cancellation → `pthread_cancel()` of
a thread blocked in `read()` hangs (and under other timing, faults → the
intermittent `tst-eintr1` segfault).

It is a **latent, unfixed upstream MicroBlaze bug**: mainline
`arch/microblaze/kernel/signal.c` is byte-for-byte identical to the audited tree,
and no upstream commit addresses this class. Nothing to cherry-pick — a fix must
be authored.

## How we got here — everything that was RULED OUT (with the test that did it)

| Hypothesis | Verdict | Evidence |
|---|---|---|
| Our entry.S argument-save patch | NOT it | reproduces on stock entry.S (workaround-GCC baseline) |
| MSR[C] lost across signals (carry/atomics) | NOT it | high-rate probe: 400 sigs, 131 in-window hits, **0** MSR[C] leaks. Also: MSR isn't stored in the sigframe at all, so a nested signal can't corrupt it |
| A lost cancellation CAS | NOT it | handler reads `cancelhandling=0x08` — CANCELED bit correctly set |
| Frame PC outside glibc's cancel bridge | NOT it | traced `framepc` **in** range |
| r19 (callee-saved) clobbered by a plain syscall | NOT it | direct probe: 8M `getpid()` under heavy IRQ load, **0** r19 corruption, wall AND icount |
| SP == &siginfo overlap corrupting si_pid in memory | secondary | reserving a 32-byte ABI arg-save area did **not** cure the hang; nested frames land *below* &info so stack spills don't write up into it |
| Kernel stack overflow / undersized stack | NOT it | bumped THREAD_SHIFT 13→14 (8K→16K) + enabled DEBUG_STACK_USAGE/SCHED_STACK_END_CHECK: **still hangs 2/2 under icount, no canary/overflow warning** |
| entry.S register save/restore asymmetry / bad frame math | NOT it | full symbolic audit: SAVE/RESTORE offset-symmetric, frame arithmetic balances, every *restore* window is IRQ-masked, MSR[C] preserved via `addik` |

Stack **direction** is correct (grows down; thread_info at base; `-PT_SIZE` on
entry). Stack **sizes** are consistent (`PT_SIZE=152=sizeof(pt_regs)`,
`THREAD_SIZE=8K`, symbolic arithmetic, `CONFIG_MB_MANAGER` off).

## What points AT the interrupts-enabled signal-return window (two independent Opus audits + dynamic data)

1. **Timing is the only variable.** Same image hangs under `qemu -icount shift=0`,
   works wall-clock; different `-icount` shifts flip it; `linux-min` hangs at every
   shift, `linux-pristine` works at most. Inserting instrumentation (even ~6
   instructions, or the trace plugin) flips it — the observer effect *is* the
   proof of a tight race. This is why userspace instruction-level tracing can't
   localize it.

2. **entry.S audit, suspicion #1:** the `do_notify_resume` loop (entry.S ~451-473)
   is the *only* place IRQs are enabled while a live `pt_regs` sits on the kernel
   stack, and it is exactly the read()+cancel path. It hands `r5=&pt_regs` to the
   C code, then re-loops (`bri 1b`) re-reading `TI_FLAGS`. A nested timer IRQ in
   that window, or non-idempotent mutation of `pt_regs->pc` across the re-loop,
   can produce a **double `pc -= 4`** (→ re-enter `read()` → hang) or a frame
   built on half-updated `pt_regs` (→ segfault). The nested-IRQ arrival point vs
   the flag re-check is exactly what `-icount` shifts.

3. **signal.c audit, suspicion #1:** `setup_rt_frame`'s `copy_siginfo_to_user`/
   `__put_user` write the *user* frame with IRQs enabled; a TLB/page-fault on
   those user addresses (software-managed TLB → frequent) re-enters via
   `page_fault_data_trap`, and both exception- and interrupt-entry stomp the
   **shared per-CPU scratch `PER_CPU(ENTRY_SP)`/`CURRENT_SAVE`** (single slots).
   A timer IRQ nesting in that narrow window corrupts the `pt_regs`/`r6`(siginfo
   ptr) that become the handler's launch context → wrong `si_pid`. This directly
   explains the plugin-observed corrupted `si_pid` (it was real, kernel-side — not
   a pure artifact).

Both audits, my `ENTRY_SP` spot-check, and the dynamic negatives converge on the
same place.

## Divergences from other arches worth fixing regardless (correctness, not the hang)

- `setup_sigcontext`/`restore_sigcontext` never save/restore **MSR** (only
  r0–r31, pc, ear, esr, fsr). Other arches round-trip the status register; on
  MicroBlaze the user resumes a signal with a kernel-derived MSR. Not the hang,
  but wrong.
- `struct rt_sigframe` has `siginfo` at offset 0 and `setup_rt_frame` sets
  `regs->r1 = frame`, so the handler runs with SP == &siginfo (no ABI arg-save
  gap). Latent; reserve the area.

## Next step to pin the exact instruction

Kernel-side, non-perturbing: watch `PER_CPU(ENTRY_SP)`/`CURRENT_SAVE` and the
`do_notify_resume` loop (`pc`, `r1`, `r30`, `TI_FLAGS` per iteration) under
`qemu -icount` via a QEMU exec trace / hardware watchpoint correlated with
`System.map`, catching the iteration where a nested IRQ/fault lands. This is the
one instrument that can localize it without perturbing the race (no guest
instructions added).

## Artifacts

Reproducers: `repro/microblaze-entry/{cancel-min,cancel-diag,pc-probe}.c`,
`brout/mb-msrtest.c` (MSR probe), `brout/mb-r19test.c` (r19 probe),
`qplug/canceltrace.c` (QEMU trace plugin). Known-hanging image: `linux-min.bin`
(deterministic under `-icount`). Symbols recovered from its embedded initramfs.

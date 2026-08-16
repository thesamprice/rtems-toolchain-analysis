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
| Shared per-CPU `ENTRY_SP`/`CURRENT_SAVE` scratch corrupted by nested exception+IRQ | NOT it | hw_exception audit: every `ENTRY_SP` writer masks IRQs for the full live window (EIP for exceptions, IE=0 for IRQ, BIP for syscall); the scratch is consumed inside the masked prologue |

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

3. **~~signal.c audit, suspicion #1: shared-scratch `ENTRY_SP` race~~ — REFUTED.**
   A dedicated `hw_exception_handler.S`+`entry.S` audit disproved this: on
   MicroBlaze `MSR[EIP]` (exception-in-progress, bit 9) masks interrupts just like
   `BIP`. `page_fault_data_trap` runs `SAVE_STATE` (which writes `ENTRY_SP`) under
   `EIP=1`, so a timer IRQ **cannot** nest into the scratch-live window. Proof:
   copy-to-user page faults are ubiquitous; if EIP didn't mask, every one would
   race `ENTRY_SP` and the kernel wouldn't boot. `unaligned_data_trap` even does
   `set_bip; clear_eip; set_ee` in that order — deliberately handing masking from
   EIP to BIP. `CURRENT_SAVE` only ever holds `current` (idempotent). The
   lightweight TLB handler uses its own `pt_pool_space`, not the shared slot.
   So the asm scratch is protected on every path (EIP / IE=0 / BIP). Ruled out.

So both audits agree the **assembly layers are clean** — no register save/restore
asymmetry, no bad frame math, no unmasked shared-scratch window. The surviving
suspect is purely at the **C level** (mechanism #2 above): the interrupts-enabled
`do_notify_resume`/`do_signal` loop and `setup_rt_frame`/`handle_restart` logic,
where a nested timer IRQ (legitimately taken during that window) can re-drive the
work-pending loop or interact with syscall-restart non-idempotently. The wrong
`si_pid` the handler observed is therefore produced by the C-level frame/restart
handling, not by an asm scratch corruption.

## Divergences from other arches worth fixing regardless (correctness, not the hang)

- `setup_sigcontext`/`restore_sigcontext` never save/restore **MSR** (only
  r0–r31, pc, ear, esr, fsr). Other arches round-trip the status register; on
  MicroBlaze the user resumes a signal with a kernel-derived MSR. Not the hang,
  but wrong.
- `struct rt_sigframe` has `siginfo` at offset 0 and `setup_rt_frame` sets
  `regs->r1 = frame`, so the handler runs with SP == &siginfo (no ABI arg-save
  gap). Latent; reserve the area.

## Next step to pin the exact instruction

The asm scratch is ruled out, so the instrument should target the **C-level
work-pending loop**. Kernel-side, non-perturbing: under `qemu -icount`, trace the
`do_notify_resume` loop (entry.S ~451-473) — log `pc`, `r1`, `r30`(in_syscall),
and `TI_FLAGS` on each `1:`→`5:` iteration, plus entry/exit of `do_signal` /
`handle_restart` / `setup_rt_frame` — via a QEMU exec trace correlated with
`System.map`. Catch the iteration where a nested timer IRQ lands and either (a)
re-drives `do_signal` so `handle_restart` applies `pc -= 4` twice (→ re-enter
`read()` → hang), or (b) builds/serves a frame on a half-updated `pt_regs`. QEMU
tracing adds no guest instructions, so it won't flip the race the way the
userspace plugin did.

## Artifacts

Reproducers: `repro/microblaze-entry/{cancel-min,cancel-diag,pc-probe}.c`,
`brout/mb-msrtest.c` (MSR probe), `brout/mb-r19test.c` (r19 probe),
`qplug/canceltrace.c` (QEMU trace plugin). Known-hanging image: `linux-min.bin`
(deterministic under `-icount`). Symbols recovered from its embedded initramfs.

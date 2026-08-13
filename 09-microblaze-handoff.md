# Handoff: MicroBlaze GCC 15 / PR 121432

Read [08-microblaze-ira-callee-save.md](08-microblaze-ira-callee-save.md) first for the analysis.
This is the operational state: what is proven, what is not, what to run next, and what to avoid.

**One-line status:** the GCC-side conclusion is solid and ready to send; the RTEMS bug is real and
patched; the Linux failure is **unresolved and the local reproduction is invalid**.

---

## 1. What is established

### GCC is not at fault. High confidence, independent of any kernel.

The MicroBlaze ABI has the caller reserve stack space for register arguments, and the callee may
spill them there. Not a reading of prose:

- `gcc/config/microblaze/microblaze.h:441` — `REG_PARM_STACK_SPACE` is
  `MAX_ARGS_IN_REGISTERS * UNITS_PER_WORD`, non-zero. This is the formal statement that the
  **caller owns and must allocate** that memory.
- `:437` — `FIRST_PARM_OFFSET` is `UNITS_PER_WORD`, so the area starts at `caller_sp + 4`.
- `microblaze.cc:2100-2117` — the frame diagram, "arguments for called subroutines" above the
  link register.

Reproduced on **GCC 12.4.1**, three majors before the "regression":

```c
extern void use (int *p);
void spill_arg (int a) { use (&a); }
```
```
	addik	r1,r1,-28
	swi	r5,r1,32        <-- caller_sp + 4, above this frame
```

Same construct as the `do_IRQ` disassembly in PR 121432 comment #11. **The codegen is not new**,
so GCC 15 is not where any defect was introduced; `3b9b8d6cfdf5` only changed how often IRA picks
it.

Deliverable: [`patches/gcc/reg-parm-stack-space.c`](patches/gcc/reg-parm-stack-space.c), a
`gcc.target/microblaze` test pinning the contract. Upstreamable on its own. Verified against
GCC 12.4.1.

**The Buildroot patch should not be upstreamed.** It ignores every parameter it is passed,
misrepresents i386 as precedent (`i386.cc:21231` is a real cost function), disables shrink-wrapping
for all MicroBlaze code, is disclaimed by its own author in comment #10, and does not fix the
kernel anyway.

### RTEMS has the same ABI bug, and it is live

Independent of Linux, and reproduced.

`_interrupt_handler.S` allocates a 56-byte frame, saves R3–MSR at offsets 0–52, passes the source
in `r5`. `_ISR_Handler` is assembly using only r3/r4, so `r5` reaches
`bsp_interrupt_dispatch(uint32_t source)` — a C function — with `r1` still at the frame base. The
callee's spill slot is therefore `MICROBLAZE_INTERRUPT_FRAME_R4`.

At `-O0` GCC really emits it:

```
	addik	r1,r1,-48
	addk	r19,r1,r0
	swi	r5,r19,52     /* caller_sp+4 == frame offset 4 == R4 */
```

Modelled under QEMU: saved R4 = `0x000000ff` (the source constant itself) unpatched,
`0xfeedface` patched.

At `-O1`+ the allocator prefers a callee-saved register, which is the only thing hiding it — the
same luck Linux had before GCC 15.

**Patch pushed:** branch `microblaze-isr-arg-area` on the GitLab fork
(`ssh://git@gitlab.rtems.org:2222/TheSamPrice/rtems.git`), commit `713412097d`. Reserves 32 bytes
at the bottom of the frame, moves the saved registers above it, and relocates the stashed
interrupted SP off offset 0 into a named slot. **Not yet built against a real BSP** — no MicroBlaze
BSP is installed locally, so it is validated by a QEMU model of the frame, not by a real ISR.

---

## 2. What is NOT established

**The local Linux reproduction is not PR 121432.** This is the important negative result.

| build | outcome |
|---|---|
| GCC 15, no workaround, stock kernel | hang after `Run /init as init process` |
| GCC 15, no workaround, entry.S patched (10 sites) | panic, SIGSEGV on init |
| GCC 15 + Buildroot workaround, stock kernel | **hang after `Run /init`** |

Comment #45 says the workaround gets you to a shell. Here it changes nothing. So this build hangs
for some other reason, and **every dynamic Linux conclusion drawn from it is void** — including
"the entry.S patch changes the failure mode", which was measured against the wrong failure.

Prime suspect: comment #45's *second* issue — stalls after init on kernels newer than 4.19 with
**any** compiler, including GCC 13 and 14. The local build is Linux **6.18.7**.

Also unestablished:

- That fixing the argument save area fixes the reported hang. `-O0 -fno-inline` on every C callee
  of `entry.S` — forcing the spill deterministically — changed **nothing**. That argues against
  the ABI mechanism being the cause, though it rests on the same invalid reproduction.
- Whether the Linux `entry.S` patch is correct. It applies to 6.18.7, builds clean, and its
  instructions assemble, but it has never been validated against a real failure.

---

## 3. The MSR lead (untested, most promising)

Sam's observation: RTEMS had a bug where MSR was not saved/restored across context switches.

Why it fits better than the ABI theory:

- MicroBlaze MSR carries `VM` (MMU), `UM` (user mode), `IE`, `BIP`, `EIP`. Wrong bits on the return
  path put you in userspace in the wrong mode — which looks exactly like "kernel boots fine, dies
  the instant it enters user space, no bad address in evidence".
- It explains the one thing the ABI theory cannot: `-O0` forcing spills everywhere changed nothing.
  If the fault were `PT_R1` corruption, `-O0` should have made it worse. If it is MSR, spills are
  irrelevant.

**Static audit done, found nothing.** Only 5 `PT_MSR` references in `entry.S` and they pair up:

```asm
SAVE_REGS:          mfs r11, rmsr ;  swi r11, r1, PT_MSR
RESTORE_REGS:       lwi r11, r1, PT_MSR ;  mts rmsr, r11
RESTORE_REGS_RTBD:  lwi r11, r1, PT_MSR ; andni MSR_EIP ; ori MSR_EE|MSR_BIP ; mts rmsr, r11
```

Not yet audited: the `clear_ums` / `set_vms` / `clear_vms_ums` macros and the `rted`/`rtbd` paths
in `hw_exception_handler.S`. A dynamic test needs a valid reproduction first.

The QEMU trace showed MSR changing across the vector
(`msr=800061a6` → `msr=800041a4`) and **nobody has checked whether the restored value matches the
saved one**. That is directly checkable and cheap.

---

## 4. The harness (working, reuse it)

Buildroot cannot run on macOS (rejects `/usr/bin/gcc` for being clang) and refuses to configure as
root. Working recipe: Debian container, deps as root, build as unprivileged user, tree on a
**named docker volume** so it survives.

| | |
|---|---|
| volume | `brtree` — holds `/br/src`, the whole Buildroot tree including toolchain |
| full build | ~33 min (toolchain + kernel + rootfs) |
| **incremental kernel rebuild** | **~4 min** |
| config | `qemu_microblazeel_mmu_defconfig`, GCC 15, Linux 6.18.7 |
| boot | `qemu-system-microblazeel -M petalogix-s3adsp1800` **inside the container** — the host has only big-endian `qemu-system-microblaze` |

Scripts in the session scratchpad (copy them somewhere permanent):

- `br-docker.sh` — full build from scratch, idempotent, skips clone/defconfig if the volume has them
- `rebuild.sh` — incremental: installs patches, `make linux-rebuild all`, boots. `O0=1` forces
  `-O0 -fno-inline` on the C callees; `TRACE=1` adds `-d int,guest_errors,unimp`
- `control4.sh` — the control: workaround compiler + pristine `entry.S`
- `patches/` — `entry.S.full` (10-site patch), `entry.S.pristine`, `fault.c.debug`,
  `gcc-workaround.patch`

Boot cmdline now includes `panic=1 print-fatal-signals=1`, and `-no-reboot` makes QEMU **exit at
the panic** instead of spinning in the panic path for the whole timeout — which is what previously
buried the useful part of the trace under tens of thousands of post-mortem interrupts.

`fault.c.debug` instruments the user-fault path with `pr_emerg("BADFAULT: ...")` + `show_regs()`,
giving the RTEMS-style register dump. **It has never successfully run** — the first attempt failed
to compile (missing `#include <linux/sched/debug.h>`, since fixed) and the runs after that were
consumed by the control. This is the single highest-value instrument available and it is ready.

---

## 5. What to do next, in order

1. **Get a valid reproduction.** Nothing dynamic means anything until Buildroot's workaround makes
   a build boot. Try the kernel version the defconfig shipped at the time of the report — comment
   #45's data suggests ≤4.19 boots. Set `BR2_LINUX_KERNEL_CUSTOM_VERSION`. **Verify by confirming
   the workaround build reaches a shell before anything else.**
2. **Run the fault dump** (`fault.c.debug`) on the *unpatched* failing build. Read `r1` — it is the
   stack pointer (`microblaze.h`: `MB_ABI_STACK_POINTER_REGNUM 1`). If it is not a plausible user
   SP, `PT_R1` was clobbered and the ABI theory is back; if it is sane, look at the dumped PC.
3. **Then** test MSR: dump `PT_MSR` on entry, compare with what is actually restored, and audit
   `clear_ums`/`set_vms`.
4. **Separately and independently:** send the GCC test, and build the RTEMS patch against a real
   `microblaze_fpga` BSP.

---

## 6. Running log (newest last — update this when handing off)

A dated record of every experiment against the `brtree` volume, so nobody re-runs or
re-trusts one of these by accident. Convention: **state the setup verification result next to
the outcome**; an outcome without one is not evidence.

| # | date | run | setup verified? | outcome |
|---|---|---|---|---|
| 1 | 08-13 | full build, GCC 15, no workaround, Linux 6.18.7, stock kernel | yes (hindsight) | hang after `Run /init` |
| 2 | 08-13 | entry.S 7-site patch, incremental rebuild | **no** — `patch --forward` skipped; then re-run with file copy: yes | panic SIGSEGV 0x0b |
| 3 | 08-13 | entry.S 10-site patch (+3 `rted` sites) | yes (33 `C_ARG_SIZE` refs confirmed in tree) | identical panic |
| 4 | 08-13 | `-O0 -fno-inline` on all C callees + 10-site patch | first attempt killed by live script edit; re-run: yes (CFLAGS confirmed in Makefile) | identical panic → **ABI spill ruled out as cause** |
| 5 | 08-13 | control v1: workaround via `package/gcc/15.*/` patch drop | **no** — Buildroot patches at extract time; 0 hook refs in built source | invalid |
| 6 | 08-13 | control v2: hook edited into extracted source | source yes, **binary no** — `host-gcc-final-rebuild` failed, stale cc1 booted | invalid |
| 7 | 08-13 | control v3: `host-gcc-final-dirclean` + rebuild | GCC built and installed (cc1 16:16), but my mtime guard checked `gcc/cc1` instead of `build/gcc/cc1` and aborted before boot | no boot, but **left the workaround compiler installed** |
| 8 | 08-13 | fault-dump build (`fault.c.debug`) | **no** — missing `#include <linux/sched/debug.h>`, kernel did not compile | invalid; include since fixed |
| 9 | 08-13 | control v4: workaround cc1 (from #7) + pristine entry.S, Linux 6.18.7 | yes (build clean, banner confirmed) | **still hangs → this reproduction is not PR 121432** |
| 10 | 08-13 | control v5: same, kernel switched to **6.12.81** (comment #45's known-good-with-workaround version) | n/a | build failed: `No hash found for linux-6.12.81.tar.xz` |
| 11 | 08-13 | control v5b: sha256 appended to `package/linux/linux.hash` + `package/linux-headers/linux-headers.hash` | **no** — those paths do not exist; the `grep` on a missing file killed the script under `set -e` before `make` ran; the "boot tail" in the monitor was run 10's stale log | invalid (died in preconditions) |
| 12 | 08-13 | control v5c: hash registered in the **real** files — current Buildroot keeps kernel hashes in versioned subdirs, `linux/before-6.17/linux.hash` and `linux/before-6.17/linux-headers.hash` — and all status written to `/out/c5c-status.txt` on the volume, because the host-side log filter has now swallowed two containers' stdout | hook refs + config + hash entries recorded in the status file pre-build | **running** |

State of the `brtree` volume after run 12 launch: workaround-patched GCC 15.3.0 installed; `.config`
pinned to custom kernel 6.12.81; both hash files carry the 6.12.81 entry; kernel tree is a fresh
6.12.81 extract (pristine — the 10-site entry.S patch was for 6.18.7 and is **not** applied).

Next planned runs, in order:
- **v5b shell** → control v6: `host-gcc-final-dirclean` + rebuild **without** the hook (verify 0
  refs in fresh source before building), `linux-dirclean`, boot → expect the genuine hang. Then
  regenerate `fault.c.debug` against the 6.12.81 tree (the 6.18 one will not apply) and run the
  dump on that failing baseline. Read `r1` first, then PC, then `PT_MSR` vs restored MSR.
- **v5b no shell** → kernel version is not the confound; pin the Buildroot tree itself to its
  ~Aug 2025 revision (the report's era) and repeat from the defconfig.

## 7. Traps — every one of these cost real time here

**Verify the setup before reading the outcome.** Four of seven failed runs produced a
plausible-looking log from a build that did not contain the change under test. A run is worthless
until the **artefact** — not the source, not the makefile — is confirmed to differ.

| trap | what happened |
|---|---|
| custom kernel versions need a registered hash | Buildroot hard-fails any tarball not listed in the package hash file, and the custom version propagates to `linux-headers` too. The hash files are **not** at `package/linux/` — current master keeps them in versioned subdirs: `linux/before-6.17/linux.hash`, `linux/from-6.17/linux.hash`, and matching `linux-headers.hash`. Get the sha256 from `https://cdn.kernel.org/pub/linux/kernel/v6.x/sha256sums.asc`. |
| container stdout is unreliable here | a host-side log filter reduced two containers' entire output to an empty summary. **Write all status to a file on the mounted volume** and read that; never diagnose from `docker logs`. |
| `patch --forward` silently skips | a file already carrying an earlier revision of the same change gets skipped; the rebuild produced a kernel without the new hunks, and the boot looked like "the extra sites made no difference". **Copy the whole patched file instead.** |
| editing a script a container is reading | bind mounts are live and bash reads incrementally; a mid-run edit produced a syntax error at whatever line the shell reached, exit 2, and left a stale log that looked like a result. **Copy the script to a frozen path before launching.** |
| Buildroot patches apply at *extract* time | dropping a `.patch` into `package/gcc/15.*/` after extraction does nothing. Patch the extracted tree directly. |
| checking the source, not the binary | the source carried the hook and the compiler was still stale, because `host-gcc-final-rebuild` had failed. Check `cc1`'s mtime — and note the real path is `build/gcc/cc1`, not `gcc/cc1`. |
| grepping for idioms, not instructions | `bralid\|brlid\|brald\|rtbd` missed three sites using **`rted`** (`full_exception`, two `do_page_fault`). |
| two return conventions in `entry.S` | where `r15` is the branch itself the callee returns after the delay slot; where `r15` is `label - 8` it returns to the label, **skipping anything in between**. A restore placed after the delay slot at such a site never executes. |
| `addi` vs `addik` | `addik` keeps carry and leaves MSR alone. Using `addi` on these paths would clobber MSR[C] on every interrupt — manufacturing the exact bug class under investigation. The current patch uses `addik` throughout. |
| QEMU `-d int` is the wrong instrument | MicroBlaze has a software-managed TLB, so every miss is an MMU exception — 21473 of them, all normal. A fatal fault and a routine page-in look identical to QEMU. **The kernel knows why it killed the task; make it say so** (`show_regs`, `print-fatal-signals=1`). |
| reading the busiest thing in a log as the interesting thing | the repeating interrupt at the trace tail was `__udelay.constprop.0` immediately before `panic_try_start` — the panic path's delay loop, i.e. *after* the fact. Resolve addresses against `System.map` before interpreting. |

**And the meta-lesson:** the control experiment — does the known-good workaround fix *this* build —
was run last instead of first. It would have cost 30 minutes at the start and saved most of what
followed.

# Handoff: MicroBlaze GCC 15 / PR 121432

Read [08-microblaze-ira-callee-save.md](08-microblaze-ira-callee-save.md) first for the analysis.
This is the operational state: what is proven, what is not, what to run next, and what to avoid.

**One-line status: SOLVED AND INTERVENTION-PROVEN.** Root cause: `entry.S` calls C with `r1` at
the `pt_regs` base at 11 sites — fatally, the syscall dispatch, where the handler's ABI
first-argument spill slot **is `PT_R1`**, so every syscall could overwrite the saved user stack
pointer (init got `AT_FDCWD` as its SP). The three-file fix (v2, 15 sites)
([`patches/linux/0001-…-TESTED.patch`](patches/linux/), 13 hunks) boots pristine GCC 15.3 +
Linux 6.12.81 to a full shell on the exact baseline that hangs stock. Five-run proof chain:
workaround→shell (14), pristine→hang (16), fault dump names `AT_FDCWD` (17), partial fix moves
the failure exactly as predicted (18), full fix boots (19). GCC needs no change; the Buildroot
workaround masks a kernel ABI violation. Remaining: package for LKML, comment on PR 121432,
and the RTEMS/GCC deliverables already queued.

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

**Update (run 14): the harness is now VALID on Linux 6.12.81** — workaround compiler + stock
kernel reaches a shell, matching comment #45. The paragraphs below record the earlier state on
Linux **6.18.7**, where the workaround changed nothing; that remains true for 6.18.7 and is
comment #45's second, compiler-independent issue. All 6.18.7-based dynamic conclusions stay void.

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

**Ask Buildroot, don't guess:** `make -s printvars VARS="LINUX_HEADERS_HASH_FILES LINUX_HASH_FILES"`
prints exactly which files the infra will consult — gate scripted changes on that output instead of
asserting a path. This ended a three-run guessing streak about hash-file layout.

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
| 12 | 08-13 | control v5c: hash appended to `linux/before-6.17/{linux,linux-headers}.hash`; status via volume file | preconditions yes, **but the guess about which hash file applies was wrong** | same `No hash found` — those subdirs are not selected for a custom version |
| 13 | 08-13 | control v5d: read `pkg-generic.mk:502` — hash lookup is `$(PKGDIR)/$(VERSION)/$(RAWNAME).hash`, a subdir named **exactly the version**. Created `linux/6.12.81/linux.hash` + `package/linux-headers/6.12.81/linux-headers.hash`, and gated the build on **`make printvars VARS=LINUX_HEADERS_HASH_FILES`** confirming selection | yes — printvars showed both files selected (plus `board/qemu/patches/…` as a second consulted dir) | hash passed; 6.12.81 downloaded and built; **new failure**: `Incorrect selection of kernel headers: expected 6.18.x, got 6.12.x` |
| 14 | 08-13 | control v5e: flip declared headers series `BR2_PACKAGE_HOST_LINUX_HEADERS_CUSTOM_6_18` → `_6_12`, `olddefconfig`, resume | series flip verified in `.config` pre-build; `Linux version 6.12.81` banner confirmed | **SHELL REACHED** — syslogd, DHCP lease, crond, `buildroot login:`. **Positive control passes; harness is valid.** The 6.18.7 hang was a separate kernel-side issue, exactly comment #45's second problem. |
| 15 | 08-13 | control v6: remove workaround via `host-gcc-final-dirclean` + rebuild, gate on hook refs = 0 after re-extract | **gate fired**: after a full 25-min rebuild, the re-extracted source still had 3 hook refs. Cause: run 5's `0003-gcc-config-microblaze-fix-ira-for-GCC15.patch` was still in `package/gcc/15.3.0/` — inert while the source stayed extracted, **applied automatically the moment dirclean re-ran the extract step**. The gate correctly refused to boot | invalid baseline prevented; 25 min spent, no bad data produced |
| 16 | 08-13 | control v6b: stray patch removed, dirclean + rebuild + gates + boot | **all gates green**: leftover patches none, hook refs after re-extract **0**, cc1 mtime changed, BUILD_RC=0, banner = pristine 15.3.0 | **NO SHELL — hang after `Run /init`. Genuine failing baseline established.** With run 14 this is a validated A/B on identical 6.12.81 kernels: workaround → shell, pristine → hang. **PR 121432 reproduced; GCC 15.3 still carries it** (15.2-pinning contingency moot) |
| 17 | 08-13 | fault dump on the failing baseline | gates green | **THE DUMP FIRED AND NAMED THE ROOT CAUSE**: `BADFAULT: pid=1 comm=init addr=0000003c`, `r1=FFFFFF9C` = **-100 = AT_FDCWD**, PC in user text, `esr=0x1012` (data TLB miss, delay slot), `SEGV_MAPERR`. Init was returned to user space with a syscall argument as its stack pointer. See "Root cause" below |
| 18 | 08-13 | fix test: 11-site patch on the failing baseline | gates green (26 refs, pristine compiler) | **no shell, and no `BADFAULT`** — the AT_FDCWD signature is gone (dispatch fix works) but init dies in the **signal path**: `rPC=BF98C968` is a *stack* address, i.e. the sigreturn trampoline. Diagnosis: `sys_rt_sigreturn_wrapper` (`entry.S:519`) computes the regs pointer as `addik r5, r1, 0`, assuming `r1` == `pt_regs` — **my dispatch fix broke that assumption** by lowering `r1` first, so sigreturn restored context from `pt_regs-32`. A bug in the fix, not a 12th kernel site |
| 19 | 08-13 | wrapper corrected to `addik r5, r1, C_ARG_SIZE`; gate 28 refs | all gates green, pristine compiler confirmed | **SHELL. FIX CONFIRMED.** Full userspace — DHCP lease, crond, `buildroot login:` — and **zero `BADFAULT` prints the entire boot**. Same compiler that hangs the stock kernel |
| 20 | 08-13 | **v2**: full r15-rule audit of every `arch/microblaze/kernel/*.S` found 4 more sites — `schedule_tail` ×2 (latent, fork child's `pt_regs`), `bad_page_fault` from `hw_exception_handler.S` (**live**: stores to `PT_R1`/`PT_R3` in this binary), `machine_early_init` from `head.S` (**live**: spills past the init stack top every boot). All covered; 3 files, gates: 32 refs + per-file checks | all gates green | **SHELL — v2 confirmed.** Per-site test coverage: site 15 executes every boot; site 12 every fork; site 13 including the restore-after-fn path PID 1 itself takes. **Site 14 is the exception: a normal boot never executes it** (kernel unaligned access with faulting fixup), so it is verified statically instead — disassembly of the built vmlinux shows `r15=0xc0001648` → `rtsd` lands on the restore at `0xc0001650`, `brid`→`c000667c`=`bad_page_fault`, `bri`→`c00057b0`=`ret_from_exc`, all symbol-checked — raw commands and output in [`evidence/microblaze-ira/site14-disassembly.txt`](evidence/microblaze-ira/site14-disassembly.txt). Dynamic coverage would need a fault-injection test. Caveat on 12/13: the boot proves the mechanics, not the protection — `schedule_tail` does not spill under this compiler, so there is nothing to protect against yet. `mcount.S` ftrace paths flagged (config off); `giveup_fpu`'s `bralid r15,0` noted as pre-existing unrelated breakage |
| 21 | 08-14 | **v4**: line-by-line review of the outgoing patch found **three sites missing** — `full_exception` and both `do_page_fault` `rted` sites, silently dropped when the 6.12 port applied a stale 11-hunk `entry-body.diff` instead of the 14-hunk regeneration (which had been written to a differently-named file). Runs 18–20 booted without them on allocator luck. Added (41 refs), label-binding audit re-run: 5 trampolines bind correctly, zero pre-existing captures | gates green at 41 | **SHELL** — and demand paging exercises the `do_page_fault` trampolines constantly, so the restored sites are dynamically validated. Site count corrected to eighteen |
| 22 | 08-14 | **Side-quest from Neal**: `qemu_microblazeel_mmu_defconfig` "broken" with QEMU 11, BE fine. Root cause: QEMU 11 unified the MicroBlaze targets — no `qemu-system-microblazeel` binary; single `qemu-system-microblaze` with machine property `endianness` **defaulting to big**. LE image on the default machine dies instantly (`qemu: fatal: Microblaze: unaligned PC=90000d1a`); BE board unaffected because its readme already uses the unified binary name. A/B verified on QEMU 11.0.2 with the run-21 v4 image: default → fatal, `endianness=little` → **login prompt**. Buildroot patch (one line in `board/qemu/microblazeel-mmu/readme.txt`, the single source post-image.sh and CI parse) sent to Neal | diagnosis A/B on host QEMU 11.0.2; **after the send**, the literal readme command (only `-display none` added for headless) was booted to the login prompt, and post-image.sh's tag-extraction sed was simulated against the patched line and yields the exact command | fixed; patch in [`patches/buildroot/`](patches/buildroot/). Not tested: a full `make` regenerating start-qemu.sh (extraction simulated), BE under QEMU 11 (Neal reports it fine; patch does not touch it), older-QEMU compat (called out as a policy note in the commit message) |
| 23 | 08-14 | **RTEMS r15-rule audit** (same class): one real instance — `bsp_interrupt_dispatch`, already patched. `_Thread_Dispatch` ×2 takes no args (safe). Boot path (`start.S` → `_crtinit` → `boot_card`/`bsp_fdt_copy`) safe **only by composition**: `start.S` reserved 16 bytes where the ABI wants 28, rescued by `_crtinit`'s own 20-byte frame. Hardened to 32 with the contract documented; assembles clean with the RTEMS toolchain | audit + assembler check | GitLab branch `microblaze-isr-arg-area` now 2 commits (`73cf9f99e6`) |
| 24 | 08-14 | **tcgcov × Linux/VM feasibility**: unmodified tcgcov plugin loaded into QEMU 11.0.2, Linux guest booted to login with it attached, valid 2.3 MB TCGCOV1 artifact (52,801 TBs, counts+edges); symbolize ran but 0 lines — defconfig `vmlinux` has **zero debug sections** (readelf-verified), so kernel coverage is blocked by config, not tooling. Plan doc (`docs/LINUX-VM.md`, 4 tiers) pushed to tcgcov main; issues #1–#8 filed incl. two QEMU plugin-API mods (context/ASID visibility, insn phys addr) | experiment + readelf | see thesamprice/tcgcov |
| 25 | 08-14 | **Hardware regression report (Sam)**: v4 kernel + no workaround boots clean, then garbage console output (`hhhh(…`) at first userspace output. Root cause: **v4 bug** — the unaligned site wrapped `_unaligned_data_exception` as a C callee; it is *assembly*, makes no C calls, never reads `r1`, and exits via direct `brai ret_from_exc`, bypassing the restore trampoline → `ret_from_exc` rebuilds state from `pt_regs−32` → corrupted registers into userspace. QEMU/glibc boots never exercised the path (site was flagged coverage-less in the run-21 row). **v5**: site fully reverted with a why-not comment; other exception callees re-verified `asmlinkage` C; 38 refs; label audit 0 captures; run 22 SHELL. Sent to Neal threaded. Definitive test = Sam's platform | run 22 gates + label audit + callee-type verification | v5 in `patches/linux/`; awaiting hardware confirmation |
| 26 | 08-14 | **tcgcov Tiers 0–2 landed** (thesamprice/tcgcov): #1 closed — 13.2% kernel line coverage from one boot (needed `DEBUG_KERNEL` gate for the DWARF choice); #2 closed — symbolize match-rate breakdown + no-DWARF warning (`elfinfo.py`, 299 tests pass); #4 closed — userspace covtest at base `0x30000000`, LF:21 LH:11 BRF:6 BRH:4, loop count=8, taken/untaken BRDA pair, 53,079 foreign addrs fenced. `-O2` constant-folding caveat documented. Remaining: #3 modules, #5–#8 (Tier 3/4 + QEMU RFCs) | boots + pytest + LCOV artifacts | evidence in tcgcov repo |
| 27 | 08-14 | **tcgcov Tier 1 landed** (#3 closed): `CONFIG_DUMMY=m`, console-marker sidecar, and two new host features — `tcgcov rebase` (runtime→link windowing, `write_cov` added to format.py) and `--section` for ET_REL `.ko` symbolization. 13/13 module-window addrs → 18 lines of `dummy.c`+headers, `dummy_xmit` count=4 (real IPv6 RS transmits). 301 tests. Remaining: #5–#8 (Tier 3/4 + the two QEMU plugin-API RFCs) | boot + pytest + rebased artifact | evidence in tcgcov repo |
| 28 | 08-14 | **QEMU context-visibility PoC built and measured** (tcgcov #6, `patches/qemu/`): implemented the drafted RFC API against qemu-10.2.4 — `QEMU_PLUGIN_EV_VCPU_CTX_CHANGED`, `qemu_plugin_vcpu_ctx_id()`, `CPUClass::plugin_ctx_id` hook, MicroBlaze `MMU_R_PID` write-path notifier, `contrib/plugins/ctxdemo.c`. 60 s Linux boot: **93 contexts, 137,579 switches** vs ~2.4 B TB execs — the callback cost model validated with numbers. Diff applies with `patch -p1` (dry-run verified). RFC still **draft, not sent**; next is a rebase onto QEMU master | local qemu build + guest boot to login + covtest ran under plugin | patch + log in tcgcov `patches/qemu/` |
| 29 | 08-14 | **tcgcov Tier 3 landed** (#5 closed): TCGCOV2 format (FORMAT.md §11, flag HAS_CTX, {ctx,addr,count}/{ctx,src,dst,count} records), plugin `ctx=on` (per-vCPU (ctx,block) tables, ctx switch breaks the edge chain), `tcgcov contexts` list/score/extract. Acceptance: cov_a+cov_b at the SAME -Ttext-segment=0x30000000, concurrent, attributed via .beacon sections (ctx 0x54/0x56); loop counts 80 vs 130, opposite branch arms, 23/87 colliding addrs with different per-context counts. 313 tests. Remaining: #6 RFC still draft-not-sent, #7, #8 | boot + per-ctx lcov diff | evidence on issue #5 |
| 30 | 08-14 | **tcgcov #7 closed with a finding, no QEMU mod**: `qemu_plugin_translate_vaddr` (API v5, stock QEMU >= 10.1) at translation time is sufficient; plugin `phys=on` lands physical records (address_kind=paddr). Full boot: 0/1,003,576 translation failures; direct-map check _start/do_IRQ/do_page_fault all hit; rebase+symbolize -> 31,505 kernel lines from a paddr artifact. Documented limits: no .ko help (vmalloc scatter), phys erases ctx beacons. RFC #6 sheds its phys half. Remaining open: #6 (draft-not-sent), #8 (Tier 4) | boot + symbol check + kernel symbolization | evidence on issue #7 |
| 31 | 08-14 | **Boot profiled with tcgcov** (10-mb-boot-profile.md): 206.8M TB execs to login; QEMU CPU has div/FPU/barrel/mul/pcmp ON but kernel+userland built USE_DIV=0/USE_FPU=0 -> ~8.8M TB execs (9.5% of non-idle boot) in soft-division, 7.5M of it ld.so's __umodsi3 (ELF hash modulo). Dyn-linking ~29%, gunzip 23%, calibrate_delay 4.6%. Recs: USE_DIV=1 + drop -mxl-soft-div, static busybox, LZ4 initramfs, lpj= | single instrumented boot, symbol attribution via System.map/nm | 10-mb-boot-profile.md |
| 32 | 08-15 | **Fixes applied and re-measured, Buildroot patch drafted (NOT SENT)**: kernel USE_DIV=1 + userland -mno-xl-soft-mul/-div + -mxl-barrel-shift (wrapper-baking trap found: BR2_TARGET_OPTIMIZATION only takes effect when the toolchain wrapper is rebuilt) + LZ4 initramfs + lpj=. Non-idle boot work 92.6M -> 48.7M TB execs (-47.4%); __umodsi3/__mulsi3 gone, strcmp -78%, do_lookup_x -74%. pattern-compare = no-op on GCC 15, dropped. Patch: patches/buildroot/0001-...-DRAFT-NOT-SENT.patch; write-up 11-buildroot-mb-hwaccel.md | 3 rebuild rounds, each disasm-verified before boot | do not mail without approval |
| 33 | 08-15 | **RTEMS dl coverage: plan + Stage R0 verified, zero RTEMS mods** (tcgcov docs/RTEMS-DL.md, examples/rtems-dl/): libdl rejects ET_DYN so an RTEMS 'so' is ET_REL per-section; TCGCOV2 tag reused as loader *generation* kills the format-v3 need. New `tcgcov modmap` (multi-window slice, loud overlap refusal). dl01 on riscv/mbv BSP + PoC QEMU: map captured via GDB at the REAL _rtld_debug_state() RT_CONSISTENT; coverage matches ground truth (entry 2, loop 5 = argc 2+3, = serial output). Next: R1 dumper, R3 plugin generations | dl01 boot + gdb capture + modmap + symbolize | evidence in tcgcov repo |
| 34 | 08-15 | **RTEMS R3 verified + plugin split per-OS**: rtl_state=/rtl_debug= mode -- plugin watches the real _rtld_debug_state(), reads r_state + r_map from guest memory, bumps a generation per RT_CONSISTENT, snapshots module maps into artifact metadata. dl01: 3 generations, gen-1 slice with map FROM METADATA (no GDB) reproduces ground truth (2/5/3, base 0x80044ae0 = GDB capture). Guest-OS code split into mirrored tcgcov-rtems.c / tcgcov-linux.c + tcgcov-internal.h; all regressions identical. Remaining: R4 reuse acceptance, R1 dumper, R2 hooks (optional) | dl01 rtl run + linux ASID boot + stock-header build | tcgcov b6de3e7 |
| 35 | 08-15 | **RTEMS R4 accepted**: dl09 = natural reuse fixture -- 4 load/unload cycles, RTL allocator returns IDENTICAL addresses each cycle; o1 window's 4 lifetimes (14 execs each) kept separate by generation, per-lifetime slices symbolize identically (12 lines x count 1; v1 would blend to 4); merged two-lifetime map refused loudly. Cross-object reuse pinned by GenerationReuseTest (unit). RTEMS-DL plan now R0/R3/R4 all verified, zero RTEMS mods; remaining optional: R1 dumper, R2 hooks, custom two-object fixture | dl02/05/07/08/09 all pass under rtl mode | tcgcov 6be9363 |
| 36 | 08-15 | **RTEMS R1 verified + LIVE cross-object reuse**: rtl-map-dump.c (~50 lines, app-side, public RTL API -- still zero RTEMS mods) prints true per-section runtime bases, cross-validating the plugin's metadata reconstruction. Custom fixture (reuse-init + size-identical pay_a/pay_b, manual link vs mbv build tree + rtems-syms two-pass): B lands at A's EXACT freed addresses; TB 0x8004a716 = count 7/pay_a.c in gen 1, count 11/pay_b.c in gen 3, maps parsed from the RTLMAP dump. RTEMS-DL plan R0-R4 ALL verified, zero RTEMS mods; R2 hooks purely optional. Gotcha logged: first-fit reuses only on matching sizes | fixture boot under rtl plugin | tcgcov 669239c |
| 37 | 08-15 | **RTEMS R2 prepared on fork, verified -- plan COMPLETE**: branch rtl-debugger-hooks (d9ca18310d, pushed to TheSamPrice/rtems, NO MR) adds rtems_rtl_debugger_load/unload per DYNAMIC-OBJECTS section 5 (30 lines incl header + spec). Verified without BSP rebuild via RTL_OVERRIDE link-ahead trick + GDB: per-object hits, load(A) ctor_run=0 -> ctor output after -> unload(A) -> load(B) -> unload(B). Submodule restored to microblaze-v-bsp. RTEMS-DL R0-R4 all done: R0/R1/R3/R4 zero-mod, R2 optional-on-fork | GDB hook trace + serial ordering | tcgcov c38115e |
| 38 | 08-15 | **Ctor window measured shut** -- plugin rtl_load= watches the R2 hook (pre-ctor generation bump + snapshot). Same exe, two runs: rtl_state-only -> ctor execs in gen 0 (empty map, unattributable); +rtl_load -> gen 1 (map has /pay_a.o), pay_a.c:14/16 pay_ctor symbolize = coverage of code running INSIDE dlopen. RTEMS-DL plan fully closed: all stages implemented + measured. Hooks branch stays fork-only (d9ca18310d, no MR) | A/B runs on ctor fixture | tcgcov c866c5d |
| 39 | 08-15 | **HARDWARE RESULT (Sam): v4 AND v5 both boot successfully; the earlier hhhh( garbage console was an INCOMPATIBLE THIRD PATCH in the stack, not v4.** Reattribution: the run-25-era 'v4 broke hardware' evidence was confounded; v5's revert of the _unaligned_data_exception wrap now rests on the static argument alone (asm callee exiting via brai bypasses the restore trampoline; wrapping it is useless-at-best, so the minimal patch wins). v5 remains the send candidate and now has hardware validation -> **LKML send is UNBLOCKED** (awaiting go), then PR 121432 comment + Buildroot workaround retirement. TODO: record WHICH patch was incompatible once known | hardware boots of v4 and v5 | reported by Sam 08-15 |
| 40 | 08-15 | **Coverage-verified regression test for v5: 14/14 entry.S ABI sites executed.** mb-abi-stress.c (one static userspace binary) drives every argument-save-area reserve site -- syscall dispatch, ptrace trace enter/leave, schedule_tail, do_page_fault data+instr, full_exception (privileged mts), do_IRQ+do_notify_resume (setitimer), sw_exception (brki r16,0x18). tcgcov mode=tb-insn + verify-sites.py (disasm-decoded, no DWARF) = 14/14; 15th (kgdb) config-gated OFF -> absent not missed. Findings: mode=tb misses mid-TB reserve insn; 5 exception/irq sites run real-mode at phys alias 0x90000000. Answers 'does the patch have a test' + upstreamable LKML evidence. Test-design lineage: RTEMS spcontext01 + x86/arm64 selftests | tcgcov example linux-entry-abi | tcgcov a2d08da |
| 41 | 08-15 | **Regression canary added, A/B proven.** sp_checked_syscall() reads r1 before/after brki r14,8 in one asm block -- SP mismatch = the bug (callee first-arg spill -> caller_sp+4 = PT_R1 = saved user SP), caught deterministically. CANARY PASS + exit 0 iff SP preserved. **A/B (same GCC15/rootfs, only entry.S differs): patched v5 -> boots, 60k SP-checked syscalls clean, CANARY PASS; pristine 6.12.81 (0 reserves) -> HANGS at 'Run /init', init SP corrupted = exact PR 121432 symptom.** Test turns red on unpatched. Build tree entry.S restored to v5 (38 refs) after the pristine experiment | patched vs pristine QEMU boots | tcgcov 665b763 |

State of the `brtree` volume after run 17 launch: workaround-patched GCC 15.3.0 installed; `.config`
pinned to custom kernel 6.12.81; both hash files carry the 6.12.81 entry; kernel tree is a fresh
6.12.81 extract (pristine — the 10-site entry.S patch was for 6.18.7 and is **not** applied).

Next planned runs, in order:
- **v5b shell** → control v6: `host-gcc-final-dirclean` + rebuild **without** the hook (verify 0
  refs in fresh source before building), `linux-dirclean`, boot → expect the genuine hang. Then
  regenerate `fault.c.debug` against the 6.12.81 tree (the 6.18 one will not apply) and run the
  dump on that failing baseline. Read `r1` first, then PC, then `PT_MSR` vs restored MSR.
- **v5b no shell** → kernel version is not the confound; pin the Buildroot tree itself to its
  ~Aug 2025 revision (the report's era) and repeat from the defconfig.

## Root cause (run 17, verified at every link)

**`entry.S`'s syscall dispatch hands every syscall handler a frame whose ABI argument save area
is `pt_regs` itself, and the first-argument spill slot is the saved user stack pointer.**

The chain, each link from an artifact:

1. **The register dump** (kernel's own `show_regs`, run 17): init dies at a user-text PC with
   `r1 = 0xFFFFFF9C` = **-100** = **`AT_FDCWD`** — the classic first argument of every `*at`
   syscall — and faults at `ear=0x3C` (`r1` + small offset), `esr=0x1012` = data TLB miss in a
   delay slot, `SEGV_MAPERR`.
2. **`PT_R1` is `pt_regs+4`** — `include/generated/asm-offsets.h`: `#define PT_R1 4`. The
   callee's first-argument spill slot is `caller_sp+4`. They are the same address.
3. **The dispatch site** — `entry.S:425`:
   ```
   	lwi	r12, r12, sys_call_table
   	addi	r15, r0, ret_from_trap-8
   	bra	r12
   ```
   Every syscall enters its C handler with `r1` still at the `pt_regs` base. This is **site 11**
   — missed by both audits because it is a bare `bra`, not `bralid/brlid/brald/rtbd/rted`.
4. **The spill is real and everywhere** — one `objdump` sweep of the failing vmlinux found
   **11,239 functions** storing through `r1` into their caller's argument area (legal: every C
   caller reserves it). Among the *direct* callees of asm sites: dozens of `__se_sys_*` spill
   `r5` (arg1) to `caller_sp+4` (`__se_sys_fchmodat`, `sys_rt_sigreturn`, `__se_sys_wait4`, …
   — see `evidence/microblaze-ira/spill-scan-direct-hits.txt`), and `full_exception` spills `r4`
   to `caller_sp+4`. Tail-call shims widen the set: a frameless `__se_sys_faccessat` branching to
   `do_faccessat` makes *that* function's spill land in `pt_regs`.
5. **Why GCC 15 exposed it** — `3b9b8d6cfdf5` made callee-saved registers expensive, so IRA
   started choosing caller-save + spill for incoming arguments. The entitlement existed since
   forever (`REG_PARM_STACK_SPACE`); GCC 15 just started using it. Every syscall then overwrites
   `PT_R1` with its own first argument; init's first `*at` call sets the user SP to `AT_FDCWD`.

This also retro-explains the two puzzles that derailed the ABI theory earlier:

- **The `-O0` experiment changed nothing** because it forced `-O0` on `irq.c`, `signal.c`,
  `exceptions.c`, `fault.c` — never on the syscall bodies (`fs/open.c` etc.), which are the
  functions the dispatch site actually calls.
- **The 10-site patch changed the failure without fixing it** because the eleventh site — the
  most-executed one — was still corrupting `PT_R1` on every syscall.

The fix (run 18, in flight) adds the dispatch site with the same trampoline pattern:

```
	addi	r15, r0, 4f-8
	addik	r1, r1, -C_ARG_SIZE
	bra	r12
4:	addik	r1, r1, C_ARG_SIZE
	bri	ret_from_trap
```

**Audit rule that would have found site 11 on day one:** enumerate asm→C call sites by looking
for **`r15` being set** (`addi/addik r15, r0, <label>` plus the link-forms `bralid/brlid/brald`),
not by grepping branch mnemonics. Setting up a return address is the one thing every call must
do; the branch instruction can be anything.

## 7. Traps — every one of these cost real time here

**Verify the setup before reading the outcome.** Four of seven failed runs produced a
plausible-looking log from a build that did not contain the change under test. A run is worthless
until the **artefact** — not the source, not the makefile — is confirmed to differ.

| trap | what happened |
|---|---|
| custom kernel versions need a registered hash | Buildroot hard-fails any tarball not listed in the package hash file, and the custom version propagates to `linux-headers` too. The hash files are **not** at `package/linux/` — current master keeps them in versioned subdirs: `linux/before-6.17/linux.hash`, `linux/from-6.17/linux.hash`, and matching `linux-headers.hash`. Get the sha256 from `https://cdn.kernel.org/pub/linux/kernel/v6.x/sha256sums.asc`. |
| failed experiments leave live artifacts | run 5 dropped the workaround into `package/gcc/15.3.0/` as a Buildroot patch. It did nothing for ten runs — until run 15's dirclean re-ran the *extract* step, which applied it, turning a "pristine rebuild" back into a workaround build. **When un-doing an experiment, remove its artifacts from every layer it touched** (package dirs, extracted trees, config), and gate on the artefact, which is what caught this. |
| `qemu -nographic` eats the calling heredoc | with stdin inherited, QEMU consumes the rest of the `sudo bash <<EOS` heredoc and echoes it into the guest serial console. Symptoms: script lines interleaved in boot logs, and the script silently ending at the QEMU invocation (truncated status files, exit 0 with missing verdict blocks). This corrupted three runs before being identified. **Always `< /dev/null` QEMU in scripts.** |
| container stdout is unreliable here | a host-side log filter reduced two containers' entire output to an empty summary. **Write all status to a file on the mounted volume** and read that; never diagnose from `docker logs`. |
| `patch --forward` silently skips | a file already carrying an earlier revision of the same change gets skipped; the rebuild produced a kernel without the new hunks, and the boot looked like "the extra sites made no difference". **Copy the whole patched file instead.** |
| editing a script a container is reading | bind mounts are live and bash reads incrementally; a mid-run edit produced a syntax error at whatever line the shell reached, exit 2, and left a stale log that looked like a result. **Copy the script to a frozen path before launching.** |
| Buildroot patches apply at *extract* time | dropping a `.patch` into `package/gcc/15.*/` after extraction does nothing. Patch the extracted tree directly. |
| verify patches with `git am`, on a populated tree | v2 of the Linux patch was generated from a scratch repo whose base commit held only `entry.S`, so the other two files came out as *creations* (`--- /dev/null`) and the patch refused to apply to any real tree. The pre-send check missed it because GNU `patch --dry-run` tolerates creation diffs where `git am` correctly fails. **Verify with the tool and tree shape the recipient will use.** |
| checking the source, not the binary | the source carried the hook and the compiler was still stale, because `host-gcc-final-rebuild` had failed. Check `cc1`'s mtime — and note the real path is `build/gcc/cc1`, not `gcc/cc1`. |
| grepping for idioms, not instructions | `bralid\|brlid\|brald\|rtbd` missed three sites using **`rted`** (`full_exception`, two `do_page_fault`). |
| two return conventions in `entry.S` | where `r15` is the branch itself the callee returns after the delay slot; where `r15` is `label - 8` it returns to the label, **skipping anything in between**. A restore placed after the delay slot at such a site never executes. |
| `addi` vs `addik` | `addik` keeps carry and leaves MSR alone. Using `addi` on these paths would clobber MSR[C] on every interrupt — manufacturing the exact bug class under investigation. The current patch uses `addik` throughout. |
| QEMU `-d int` is the wrong instrument | MicroBlaze has a software-managed TLB, so every miss is an MMU exception — 21473 of them, all normal. A fatal fault and a routine page-in look identical to QEMU. **The kernel knows why it killed the task; make it say so** (`show_regs`, `print-fatal-signals=1`). |
| reading the busiest thing in a log as the interesting thing | the repeating interrupt at the trace tail was `__udelay.constprop.0` immediately before `panic_try_start` — the panic path's delay loop, i.e. *after* the fact. Resolve addresses against `System.map` before interpreting. |

**And the meta-lesson:** the control experiment — does the known-good workaround fix *this* build —
was run last instead of first. It would have cost 30 minutes at the start and saved most of what
followed.

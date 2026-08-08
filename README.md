# RTEMS and its toolchain — how the GNU port works, and what an LLVM port would take

An analysis of how RTEMS integrates with GCC, binutils and newlib, and a scoped estimate of the
work needed to do the equivalent in LLVM/Clang.

## Contents

| | |
|---|---|
| [01](01-how-rtems-builds-its-toolchain.md) | **How RTEMS builds and patches its toolchain** — the RTEMS Source Builder, its config language, and a full inventory of the 42 patches it applies |
| [02](02-rtems-in-the-gnu-toolchain.md) | **What RTEMS support consists of in GCC, binutils and newlib** — measured, file by file |
| [03](03-rtems-side-clang-support.md) | **What RTEMS already has for Clang** — the 2020 scaffolding, and what Clang does with an RTEMS triple today |
| [04](04-llvm-gap-analysis.md) | **What it would take to support RTEMS in LLVM** — the gap, work items, sizes |
| [05](05-clang-riscv-bringup.md) | **Building RTEMS for RISC-V with Clang** — the experiment, and the twelve things that broke |
| [`patches/`](patches/) | The three real fixes: one Clang, two RTEMS |
| [`repro/`](repro/) | The `config.ini` carrying the remaining workarounds |
| [`results/`](results/) | Raw QEMU testsuite output |
| [`evidence/`](evidence/) | Commands run and their raw output |

## The findings, in short

**RTEMS does not fork GCC or binutils.** It builds upstream releases from a recipe, applying
patches by URL with SHA-512 pinning. Of 13 architectures in RTEMS 7, **11 use entirely stock
upstream tools**. MicroBlaze is the sole architecture with a genuinely non-upstream toolchain —
26 architecture patches plus 3 RTEMS ones, pinned to GCC 12.4.1 and binutils 2.36.1, three and
five years behind the RTEMS default respectively.

**The toolchain contains no RTEMS code. It contains a set of promises about RTEMS.** GCC's specs
promise `-lrtemsbsp -lrtemscpu` exist and a `linkcmds` will be found. `gthr-rtems.h` promises
`_Mutex_Acquire` exists. newlib promises `__getreent` exists. Every one of those promises is
fulfilled by the RTEMS kernel tree, not by the toolchain. The seam is a single driver flag,
`-qrtems`: without it GCC links a fake `crt0.o` full of stubs so that autoconf link probes
succeed against a kernel that is not there; with it, the real BSP takes over.

**The architecture-independent OS work in GCC is about 300 lines, and it was written once.**
A 13-line `config.gcc` stanza, a 24-line spec header, a 15-line options file, and a 247-line
thread-model header. Each new architecture then costs 35–80 lines. binutils needs about 30 lines
of configuration and **no source files at all** — `gas/configure.tgt` contains the line
`*-*-elf | *-*-rtems* | *-*-sysv4*) fmt=elf ;;`, which is the entire relationship: from
binutils' perspective RTEMS is a synonym for bare ELF. newlib's port directory is 8,588 lines but
**~95% headers**, with 6 lines of executable C and roughly 500 lines of real content.

**On the LLVM side, the front end already knows about RTEMS; the driver does not.** Joel
Sherrill's 2011 work added `Triple::RTEMS`, an `RTEMSTargetInfo` that defines `__rtems__`, and a
rule keeping `/usr/include` off the search path. All of that still works. But
`clang/lib/Driver/Driver.cpp` contains **zero** references to `Triple::RTEMS`, so an RTEMS triple
falls through to a generic path that hands the link step to whatever `gcc` is on `PATH`:

```
$ clang --target=arm-unknown-rtems7 -### hello.c     # last line
 "/usr/bin/gcc" "-o" "a.out" "/var/folders/.../rt-53be83.o"
```

That is the macOS host compiler being handed an ARM object.

**RTEMS's own build system is further along than LLVM is.** `wscript` already offers
`--rtems-compiler=clang`; `spec/build/cpukit/optclang.yml` sets `CC=clang`, `AR=llvm-ar` and a
correct `--target=` triple. Two architecture families (riscv, sparc/leon3) have Clang ABI flag
sets. What the Clang path lacks is the `-qrtems` equivalent — and, notably, `RTEMSTargetInfo` is
wired up for ARM, MIPS, PowerPC and SPARC but **not RISC-V**, which is the architecture RTEMS's
Clang scaffolding most targets.

**The single most load-bearing piece of work is one file**: a
`clang/lib/Driver/ToolChains/RTEMS.cpp` replicating `-qrtems`. Comparable ToolChains run 289
(Haiku) to 674 (BareMetal) lines. That plus a three-line registration in `Driver.cpp` is enough
for Clang to be a drop-in replacement for GCC using GNU binutils and RTEMS's newlib — no
compiler-rt, no lld, no libc++ required.

**The runtime libraries are in better shape than expected.** Most threading and libc dispatch is
feature-gated rather than OS-gated, and `RUNTIMES_USE_LIBC=newlib` is already first-class in
libc++. The highest impact-to-effort item in the whole assessment is **one line** in
`libcxx/include/__config`: adding `defined(__rtems__)` to the pthread-threading allowlist that
today `#error`s with "No thread API". Because libc++abi consumes libc++'s thread support header
rather than having its own, that single `#error` gates the entire C++ runtime stack. The list
already contains `__NuttX__`, another RTOS.

**One silent-failure risk is worth flagging on its own.**
`compiler-rt/lib/builtins/clear_cache.c` has no RTEMS branch and falls back to
`compilerrt_abort()` on ARM32 and RISC-V — a runtime abort, not a build error.

**Architecture coverage is a hard limit.** Upstream LLVM has backends for 9 of RTEMS 7's 13
architectures. It is missing microblaze (removed in 2013), moxie, nios2 and or1k. An LLVM RTEMS
port could cover arm, aarch64, riscv, x86_64 and sparc — which is where RTEMS's existing Clang
work already points — but could not replace GCC across the whole project.

## We then actually did it

Rather than leave [04](04-llvm-gap-analysis.md) as a paper study, we built Clang with the RISC-V
backend and pointed RTEMS 7's build system at it. **RTEMS boots and runs on QEMU built entirely
with Clang** — but only after twelve distinct fixes. [05](05-clang-riscv-bringup.md) records every
one in the order it occurs.

Three deserve highlighting:

- **Clang did not define `__rtems__` for RISC-V.** Suspected in the paper study, confirmed by
  measurement, and fixed: `Targets.cpp` had no `Triple::RTEMS` case for `riscv32`/`riscv64`. Two
  `case` arms and a regression test, directly upstreamable.
- **Two RTEMS bugs in code that had provably never been run.** `optarchbits.yml` omits four of its
  own RISC-V BSPs, so the clang triple came out as the invalid `riscv-unknown-rtems7`. And because
  RTEMS appends `--target=` *after* waf loads the compiler tools, waf probes Clang as a *host*
  compiler and overwrites RTEMS's `DEST_OS="rtems"` with `"darwin"` — which made every compile
  receive `-arch r -arch i -arch s -arch c -arch v`, waf iterating the string `"riscv"` character
  by character.
- **The `crt0.o` collision** is the single best artifact in this study. Clang linked newlib's
  *stub* `crt0.o` — the fake-kernel one whose entire purpose is to make autoconf link probes
  succeed — and it collided with the real RTEMS kernel. GCC avoids this only because its spec says
  `%{!qrtems:crt0%O%s}`. That is the `-qrtems` seam failing in the most literal way possible.

A cautionary note is recorded there too: at one stage 702 of 721 executables linked cleanly and
were **all unbootable**, because nothing passed `-T linkcmds` and lld had used its default layout.
A clean link is not evidence of a working image.

## The asymmetry

GCC's RTEMS support is **large in aggregate but tiny per decision**: a few hundred lines of specs
written fifteen years ago that every subsequent architecture inherits for free. LLVM's missing
piece is the opposite shape — a single concentrated component, the driver ToolChain, which does
not exist at all, and behind it a scattering of one-line allowlist entries.

## Method and honesty note

Findings were established by reading and running against actual trees, not from documentation:
RTEMS Source Builder `105f43d`, RTEMS 7, GCC 15.2.0, binutils-gdb master, newlib `7d4336cf`, and
`llvm-project` `0594c0187`. Where a claim rests on a code survey rather than something executed,
the document says so — see the "Verification status" section of [04](04-llvm-gap-analysis.md).

[05](05-clang-riscv-bringup.md) is entirely empirical: every failure listed there was observed,
and the fix for it was applied and re-run. The RTEMS work was done in a `git worktree` so the
existing GCC build tree was never touched, and a GCC control build was run in that worktree first
to prove the setup before changing compilers.

Caveats that matter: the Tier A/B size estimates in [04](04-llvm-gap-analysis.md) remain
judgement, not measurement — no ToolChain has been written. The bring-up in
[05](05-clang-riscv-bringup.md) still uses GCC's newlib, libgcc and libstdc++, so it demonstrates
that *Clang can compile and link RTEMS*, not that LLVM's own runtime stack works. And its
testsuite numbers carry twelve workarounds, so they are not a fair comparison against the GCC
baseline.

One correction is recorded rather than quietly fixed: the first draft of
[04](04-llvm-gap-analysis.md) generalised from ARM and claimed Clang hands RTEMS links to the host
`gcc`. That is false for RISC-V, which the driver routes to `BareMetal` by architecture before any
OS check. The original claim and its correction are both left in place.

## Related

Companion repositories from the same investigation:

- [`rtems-microblaze-linker-relaxation-bug`](https://github.com/thesamprice/rtems-microblaze-linker-relaxation-bug)
  — four binutils patches for MicroBlaze, with testsuite and torture-suite results
- [`rtems-kasan`](https://github.com/thesamprice/rtems-kasan) — a KASAN-style shadow-memory
  allocator checker for RTEMS

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

## The asymmetry

GCC's RTEMS support is **large in aggregate but tiny per decision**: a few hundred lines of specs
written fifteen years ago that every subsequent architecture inherits for free. LLVM's missing
piece is the opposite shape — a single concentrated component, the driver ToolChain, which does
not exist at all, and behind it a scattering of one-line allowlist entries.

## Method and honesty note

Findings were established by reading and running against actual trees, not from documentation:
RTEMS Source Builder `105f43d`, RTEMS 7, GCC 15.2.0, binutils-gdb master, newlib `7d4336cf`, and
`llvm-project` `0594c0187`. Where a claim rests on a code survey rather than something executed,
the document says so — see the "Verification status" section of
[04](04-llvm-gap-analysis.md). No part of the proposed LLVM work has been prototyped, so the size
estimates are informed judgement, not measurement.

## Related

Companion repositories from the same investigation:

- [`rtems-microblaze-linker-relaxation-bug`](https://github.com/thesamprice/rtems-microblaze-linker-relaxation-bug)
  — four binutils patches for MicroBlaze, with testsuite and torture-suite results
- [`rtems-kasan`](https://github.com/thesamprice/rtems-kasan) — a KASAN-style shadow-memory
  allocator checker for RTEMS

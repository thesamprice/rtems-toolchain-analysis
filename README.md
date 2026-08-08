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
| [06](06-libdl-and-lld.md) | **Getting the `dl` tests working** — a TLS model mismatch, and where lld and GNU ld disagree |
| [07](07-reaching-gcc-parity.md) | **Reaching parity with GCC** — the last eight failures, and why six of them were not compiler bugs |
| [BUGS.md](BUGS.md) | **Every bug found — fixed, worked around, and still open** |
| [`patches/`](patches/) | The fixes: three LLVM/lld, eleven RTEMS, two rtems-tools, one QEMU |
| [`repro/`](repro/) | The `config.ini` carrying the remaining workarounds |
| [`results/`](results/) | Raw QEMU testsuite output |
| [`evidence/`](evidence/) | Commands run and their raw output |
| [`tools/`](tools/) | `run-all.sh`, the identical-treatment test runner the parity claim rests on |

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
wired up for ARM, MIPS, PowerPC, SPARC and x86-32 but **not RISC-V**, which is the architecture
RTEMS's Clang scaffolding most targets.

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

## And then it matched GCC

[07](07-reaching-gcc-parity.md) closes the last eight failures. On the same 674 tests, run through
the same harness, **both compilers now produce the same verdict on every test** — `diff` of the two
result files is empty.

| | PASS | XFAIL | SKIP | FAIL |
|---|---:|---:|---:|---:|
| GCC | 634 | 25 | 9 | 6 |
| Clang | 634 | 25 | 9 | 6 |

Both columns come from trees carrying the same BSP configuration, run by the same script, and the
two results files are byte-identical.

The seven remaining failures fail identically under GCC. **Parity means matching, not zero** — and
they are now explained rather than assumed. **Both** build trees were run through RTEMS's own
`rtems-test`: four of the seven pass outright on both compilers and were only ever artifacts of the
runner's 25-second cap; `sptimecounter03` is a ten-guest-second test that emits nothing for its
whole duration, so against an inactivity-based timeout it is flaky under parallel load rather than
failing — measured concurrently it takes 45 s under clang and 50 s under GCC, and both reach
`END OF TEST`; and `dl06` and `ttest01` are genuine RTEMS failures with byte-identical output under
both compilers.

Doing that turned up a structural problem worth knowing about: **the `amd-microblaze-v-generic`
machine does not terminate QEMU when RTEMS shuts down**, so `rtems-test` cannot tell a test that
failed and shut down from one that hung — both are reported as `timeout` — and every test costs its
full timeout no matter how fast it finishes. There was also no `rtems-test` BSP configuration for
`riscv/mbv`; one is included in [`patches/rtems-tools/`](patches/rtems-tools/).

That one is now fixed rather than just reported. The machine had **no poweroff device of any kind**,
so the BSP's spinning `bsp_reset()` was the only thing it could do. Adding a SiFive test finisher to
the machine, teaching the BSP to find it in the device tree, and actually getting the device tree to
the BSP — the step that is easy to miss, since mbv falls back to an empty tree and QEMU supplies
none — takes `hello.exe` from occupying the runner for its full timeout to **exiting in 74 ms**, and
a full 674-test sweep to **5m44s**. No test verdict changes. Patches in
[`patches/qemu/`](patches/qemu/) and [`patches/rtems/`](patches/rtems/).

The breakdown of the eight root causes is the actual finding, and it is not what I expected going
in. **Two were compiler bugs. Six were not.**

- **Clang miscompiles `ctermid`.** `BuildLibCalls.cpp` infers `captures(none)` on an argument that
  POSIX says is returned, so a caller comparing the result against its own buffer folds to false.
  Target-independent — riscv32, riscv64, x86_64, aarch64, microblazeel — and wrong against any
  POSIX libc. Nothing to do with RTEMS.
- **lld does not propagate TLS alignment under a linker script.** GNU ld aligns the first TLS
  output section to the maximum alignment of all of them, in `_bfd_elf_tls_setup()`; RTEMS names
  that function in a source comment because it depends on the behaviour. lld does the equivalent
  only when it computes program headers itself.
- **RTEMS's dynamic loader passes a section index where a symbol type belongs**, so any deferred
  relocation against a symbol in ELF section index 3 is silently skipped and reported as applied.
  Clang put the symbol at index 3; GCC put it at 5. **A live latent bug for GCC users.**
- **`rtems-syms` treated undefined symbols as definitions**, because GNU ld drops unresolved weak
  undefined symbols from a linked image and lld keeps them. Root cause was in `rtemstoolkit`, one
  level below where it was first reported.
- **`ALIGN(8)` was substituted for `ALIGN_WITH_INPUT`** in the 2020 Clang scaffolding. The two are
  not equivalent — the latter imposes no alignment at all — and the substitution corrupts the TLS
  block size.
- **Three RTEMS tests depended on things they should not**: an uninitialized `struct stat` that
  GCC's stack layout happened to make benign, a constructor-ordering counter a compiler is free to
  evaluate at translation time, and a local register variable read outside an asm operand.

Two of the diagnoses recorded in [06](06-libdl-and-lld.md) turned out to be wrong and are corrected
in [BUGS.md](BUGS.md) with the original reasoning left in place. One failure was also
self-inflicted: a workaround that deleted rows from a symbol table silently renumbered the
positional TLS indices that the same file declares, manufacturing a failure that looked exactly
like a compiler bug.

## Tier A, demonstrated

The cheapest useful configuration in [04](04-llvm-gap-analysis.md) — Clang as a drop-in for GCC,
using GNU binutils and RTEMS's newlib, with no compiler-rt, no lld and no libc++ — is no longer an
estimate. **720 of 721 executables link with `riscv-rtems7-ld` and the suite passes 632 tests.**

Getting there needed one flag. GNU ld makes a single pass over each archive, RTEMS's libraries are
mutually dependent, and GCC hides that inside `-qrtems`, whose spec is
`--start-group -lrtemsbsp -lrtemscpu -latomic -lc -lgcc --end-group`. The Clang link line has no
group, so symbols first needed after an archive had been scanned were never found. lld rescans and
never noticed. The failure surfaced as `DWARF error: mangled line number section`, which was GNU ld
attaching line numbers to the undefined-reference errors it was already emitting — the real errors
had scrolled past, which is exactly what [BUGS.md](BUGS.md) O2 suspected.

The one executable that still does not link is not a linker problem: `spcxx01`'s aligned-`new` path
references `_memalign_r`, which this newlib does not define at all. lld's `--gc-sections` had been
discarding the reference; GNU ld keeps it. A latent RTEMS/newlib gap that lld was hiding.

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
judgement, not measurement — no ToolChain has been written. The bring-up still uses GCC's newlib,
libgcc, libgcc_eh and libstdc++ throughout, so it demonstrates that *Clang can compile and link
RTEMS*, not that LLVM's own runtime stack works — the only LLVM runtime component in the picture is
`libclang_rt.builtins.a`.

The parity result in [07](07-reaching-gcc-parity.md) carries its own caveats, stated there in
full: the harness is ours rather than RTEMS's own `rtems-test` — though the seven shared failures
have since been checked against it and explained — the 25 XFAILs are unaudited, and everything
`-qrtems` does is still hand-rolled in [`repro/config.ini`](repro/config.ini) with machine-specific
absolute paths. The earlier testsuite numbers in [05](05-clang-riscv-bringup.md) and
[06](06-libdl-and-lld.md) were **not** a fair comparison against GCC — different denominators and
a set of stale test binaries that were not being rebuilt — which is why [07](07-reaching-gcc-parity.md)
starts by redoing the measurement rather than by fixing anything.

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

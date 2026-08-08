# Building RTEMS for RISC-V with Clang: what actually breaks

Documents [01](01-how-rtems-builds-its-toolchain.md)–[04](04-llvm-gap-analysis.md) were a paper
study. This one is the experiment: take RTEMS 7, point its build system at Clang instead of GCC,
and record every single thing that goes wrong, in order.

The short version: **it works** — RTEMS boots and runs on QEMU built entirely with Clang — but
only after twelve distinct fixes, each of which pinpoints a specific missing piece of the
RTEMS/LLVM integration.

## Setup

| | |
|---|---|
| RTEMS | 7.0.0, BSP `riscv/mbv` (rv32imafc_zicsr_zifencei / ilp32f) on QEMU `amd-microblaze-v-generic` |
| Compiler | Clang 23.0.0git, built from source with `LLVM_TARGETS_TO_BUILD` including `RISCV` |
| Linker | `ld.lld` from the same tree |
| C library | RTEMS's own newlib, from the GCC 15.2.0 RSB toolchain — **not** rebuilt |
| GCC baseline | 517 PASS / 24 XFAIL / 2 XFAIL-KNOWN / 0 FAIL, 721 executables |

Selecting Clang is a per-BSP key in `config.ini` — **not** `--rtems-compiler=clang`, which only
affects the `bspdefaults` command and is explicitly rejected by `configure`:

```ini
[riscv/mbv]
COMPILER = clang
```

All work was done in a `git worktree` at the baseline commit so the existing GCC build tree was
never touched. A GCC control build was run in that worktree first, to prove the setup itself was
sound before changing compilers.

## The twelve failures, in the order they occur

### 1. Clang doesn't define `__rtems__` for RISC-V — *fixed in LLVM*

`clang/lib/Basic/Targets.cpp` had no `Triple::RTEMS` case for `riscv32`/`riscv64`, so they fell
to `default:` and got a plain `RISCV{32,64}TargetInfo`. Since RTEMS is gated on `__rtems__`
throughout, nothing could build.

Fixed with two `case` arms plus a regression test. See
[04, Fix 1](04-llvm-gap-analysis.md#fix-1-riscv-rtemstargetinfo-done). Upstreamable as-is.

### 2. Every compile gets `-arch r -arch i -arch s -arch c -arch v` — *fixed in RTEMS*

The most surprising failure, and a genuine RTEMS bug rather than an LLVM one.

`wscript:1568` sets `conf.env["DEST_OS"] = "rtems"`. But `optclang.yml` then runs
`conf.load(... clang ...)`, and waf's `get_cc_version()` **overwrites** `DEST_OS` from the
compiler's own predefined macros. Because RTEMS appends `--target=` to `ABI_FLAGS` *after* the
tools are loaded, waf probes Clang as a **host** compiler. On macOS it sees `__APPLE__` /
`__MACH__` and sets `DEST_OS='darwin'`, which triggers waf's `gcc_modifier_darwin()`:

```python
v.ARCH_ST=['-arch']
```

and waf's compile rule is `${CC} ${ARCH_ST:ARCH} ...`, which emits one `-arch` per element of
`env.ARCH`. RTEMS stores `ARCH` as the *string* `"riscv"`, so waf iterates it character by
character.

GCC escapes this only by accident: `riscv-rtems7-gcc` defines `__rtems__` and not `__APPLE__`,
and waf's `MACRO_TO_DESTOS` table has no `__rtems__` entry, so nothing matches and RTEMS's
`DEST_OS` survives untouched.

Fixed by restoring the target settings in `optclang.yml` after the tool load. On a Linux host the
`-arch` symptom would not appear (only the darwin modifier sets `ARCH_ST`), but the `DEST_OS`
misdetection is universal, so this is worth fixing regardless of platform.

### 3. `ARCH_BITS` is empty for `riscv/mbv` → invalid triple — *fixed in RTEMS*

`optclang.yml` builds the triple as `${ARCH}${ARCH_BITS}-unknown-rtems${__RTEMS_MAJOR__}`.
`spec/build/cpukit/optarchbits.yml` enumerates RISC-V BSPs explicitly and **`riscv/mbv`,
`riscv/mbv64`, `riscv/esp32` and `riscv/niosv` are all missing**, so they hit the
`enabled-by: true → value: ''` fallback and produce `riscv-unknown-rtems7` — not a valid triple.
Clang responds with `unsupported option '-march='`.

This is direct evidence that the RISC-V Clang path has never been exercised: the BSPs added after
2022 were simply never registered.

Fixed by adding `riscv/mbv` to the 32-bit list and `riscv/mbv64` to the 64-bit list. `esp32` and
`niosv` are left alone — I have not built them and will not guess.

### 4. `-Wold-style-declaration` is GCC-only

`optwarncc.yml` adds it unconditionally, and `optwarn.yml` adds `-Werror`. Clang: *"unknown
warning option '-Wold-style-declaration'"*. Worked around in `config.ini`; the proper fix is a
clang-conditional warning spec, which does not exist.

### 5. No sysroot — newlib headers not found

Nothing in the Clang path points at the RTEMS libc. GCC gets this from being a target-configured
compiler with a baked-in sysroot; Clang is a native compiler being cross-driven and needs
`--sysroot=` explicitly. This is one of the things a real RTEMS ToolChain would compute.

### 6. `gcov.h` not found

RTEMS's `test-gcov.h` includes it. It is a **GCC-provided** header, not a libc one, so it is
absent from the sysroot. Worked around with `-idirafter` into GCC's include dir, which keeps
Clang's own builtin headers winning.

### 7. C++ headers not found

`stdexcept` etc. live in GCC's `include/c++` tree. Clang++ also defaults to **libc++**, which does
not exist for RTEMS — the exact gap described in [04](04-llvm-gap-analysis.md#runtime-libraries).
Worked around with `-stdlib=libstdc++` plus three `-isystem` paths including the multilib-specific
`c++config.h` directory.

### 8. Clang invokes the **host** linker

The first link failed with `ld: unknown options: --sysroot=... -Bstatic --start-group --end-group`
— that is **Apple's** `ld`, which cannot link RISC-V ELF at all.

Clang's `BareMetal` ToolChain resolves a bare `ld` off `PATH`. Note that `-fuse-ld=lld` in
`LDFLAGS` did **not** fix this, because RTEMS's incremental-link (`-r`) tasks pass only
`ABI_FLAGS`, not `LDFLAGS`. Worked around with a `PATH` shim mapping `ld` → `ld.lld`.

Related: `optclang.yml` never sets `LD`, while `optgcc.yml` sets `${PROGRAM_PREFIX}ld`.

### 9. `libclang_rt.builtins.a` does not exist for RTEMS

Predicted in [04](04-llvm-gap-analysis.md). Worked around with `-rtlib=libgcc` and an explicit
`-L` into GCC's multilib directory. Building compiler-rt builtins for `riscv32-unknown-rtems7` is
the proper fix and is Tier B work.

### 10. The `crt0.o` collision — the best confirmation in this whole study

```
ld.lld: error: duplicate symbol: _Semaphore_Wait
>>> defined at lock.h:307
>>>            .../riscv-rtems7/lib/crt0.o:(_Semaphore_Wait)
>>> defined at cpu.h:154
>>>            semaphore.c.59.o:(._Semaphore_Wait) in archive ./librtemscpu.a
```

This is **exactly** the mechanism described in [02](02-rtems-in-the-gnu-toolchain.md). newlib's
`crt0.o` is the stub full of fake symbols that exists so autoconf link probes succeed against a
kernel that isn't there. GCC's spec suppresses it precisely when `-qrtems` is given:

```
*startfile:
%{!qrtems:crt0%O%s} %{qrtems:crti%O%s crtbegin%O%s}
```

Clang has no `-qrtems`, so `BareMetal` adds `crt0.o` unconditionally, and the fake kernel collides
with the real one. Worked around with `-nostartfiles`.

If you want one concrete artifact showing why an RTEMS ToolChain is load-bearing rather than
cosmetic, this is it.

### 11. Wrong multilib selected

With the stub gone, lld reported *"cannot link object files with different floating-point ABI"* —
Clang had picked `<sysroot>/lib/libc.a` (soft-float `rv32i/ilp32`) rather than the
`rv32imafc/ilp32f` multilib. Clang does not understand GCC's multilib layout for this sysroot.
Worked around with an explicit `-L`.

This one is quietly dangerous: it was caught only because lld checks ABI tags.

### 12. No `-T linkcmds` — a successful link that produces an unbootable image

At this point **702 of 721 executables "built"**, and it would have been easy to call that a
97.4% success. It was not one. Every binary was garbage:

```
              clang                      gcc (baseline)
Entry point   0x0                        0x80000000
LOAD          0x00010000, 0xcc bytes     0x80000000, 0x152a8 bytes
```

lld had used its own default layout. The first testsuite run I launched produced **empty logs** —
no UART output at all — and would have taken about four and a half hours to time out 543 tests at
120 s each before reporting a uniform failure.

The cause is the other half of `-qrtems`:

```
*endfile:
%{qrtems:crtend%O%s crtn%O%s %{!qnolinkcmds:-T linkcmds%s}}
```

RTEMS *generates* `build/<variant>/linkcmds` under every compiler, but for RISC-V nothing ever
passes `-T` — the GCC driver supplies it. Adding `-T linkcmds` to `LDFLAGS` fixed it. (RTEMS's
`spec/build/bsps/optclang.yml` already rewrites the script's `STARTUP(start.o)` to
`INPUT(start.o)` for lld, so the rest of the linker script was already clang-ready.)

The lesson worth keeping: **a clean link is not evidence of a working image.** The failure mode
here was silent, and the check that caught it was two lines of `readelf` against the baseline.

## Result

With `-T linkcmds` added: **712 of 721 executables built**, entry point `0x80000000`, and RTEMS
boots on QEMU:

```
*** BEGIN OF TEST HELLO WORLD ***
*** TEST TOOLS: Clang 23.0.0git (...f3fdc3597a...)
Hello World
*** END OF TEST HELLO WORLD ***
```

Remaining build failures, not chased:
- the `dl*` dynamic-loading tests — `rtems-ld` (RTEMS's own RAP linker) can't find the multilib
  `libm.a`; it takes its own search paths, which the `config.ini` workarounds don't reach
- one TLS link failure (`_TLS_Configuration`)

### Testsuite on QEMU

See [`results/`](results/) for the raw run. Numbers there are **not** a fair apples-to-apples
comparison with the GCC baseline: the build carries twelve workarounds, uses GCC's newlib,
libgcc and libstdc++, and is missing the `dl*` executables.

## What this tells you

The paper study in [04](04-llvm-gap-analysis.md) said the load-bearing missing piece was a driver
ToolChain replicating `-qrtems`. This experiment supports that, and sharpens it:

**Of the eleven failures, exactly one (#1) was a missing Clang feature.** Two (#2, #3) were RTEMS
bugs in code that had clearly never been run. The remaining eight were all *the same failure* in
different clothes — nobody computes the RTEMS-specific paths and link recipe. Every one of
`--sysroot`, the multilib `-L`, `-nostartfiles`, `-rtlib`, the C++ include paths, and the linker
choice is something GCC derives from being a target-configured compiler plus one spec file.

That is precisely the job of a `clang/lib/Driver/ToolChains/RTEMS.cpp`. The eleven-line
`config.ini` in this experiment is, in effect, a hand-written prototype of that ToolChain — which
also means the ToolChain's required behaviour is now empirically known rather than guessed.

Two corrections to the earlier documents fall out of this work:

1. The claim that Clang "hands the link to the host `gcc`" is architecture-dependent and false for
   RISC-V — see [the correction in 04](04-llvm-gap-analysis.md#correction-the-driver-gap-is-architecture-dependent).
2. RTEMS-side clang support is not merely "vestigial" in the abstract; it is **provably unrun** on
   RISC-V, because `optarchbits.yml` cannot produce a valid triple for four of its RISC-V BSPs.

## Reproducing

The RTEMS spec changes (#2, #3) are in [`patches/rtems/`](patches/rtems/); the Clang change (#1)
is in [`patches/llvm/`](patches/llvm/). The `config.ini` carrying workarounds #4–#11 is in
[`repro/`](repro/) with `$HOME` placeholders.

Workarounds #4–#11 are deliberately **not** proposed as patches. They are evidence of what a
ToolChain must do, not a fix.

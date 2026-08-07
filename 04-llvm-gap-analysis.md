# What it would take to support RTEMS in LLVM

Measured against `llvm-project` at `0594c0187`.

## The one-sentence answer

**Clang's front end already knows about RTEMS. Clang's driver does not.** The 2011 work added
target-info plumbing and never added a ToolChain, so today an RTEMS triple compiles correctly
and then hands the link step to whatever `gcc` is on `PATH`.

## What exists today

`Triple::RTEMS` is a real, parsed OS:

```cpp
// llvm/lib/TargetParser/Triple.cpp:945
      .StartsWith("rtems", Triple::RTEMS)
```

Sixteen non-test references exist. They fall into three groups.

### 1. Target info — the useful part

`clang/lib/Basic/Targets/OSTargets.h:630-643`, in full:

```cpp
class LLVM_LIBRARY_VISIBILITY RTEMSTargetInfo : public OSTargetInfo<Target> {
protected:
  void getOSDefines(const LangOptions &Opts, const llvm::Triple &Triple,
                    MacroBuilder &Builder) const override {
    // RTEMS defines; list based off of gcc output

    Builder.defineMacro("__rtems__");
    if (Opts.CPlusPlus)
      Builder.defineMacro("_GNU_SOURCE");
  }
```

That is why `clang --target=arm-unknown-rtems7 -dM -E -` really does emit `#define __rtems__ 1`.
**This is not a gap** — I checked before assuming it was.

But it is wired up for only **9 sites in `Targets.cpp`**, covering roughly five architectures:

```
  RTEMSTargetInfo<ARMleTargetInfo>      RTEMSTargetInfo<PPC32TargetInfo>
  RTEMSTargetInfo<ARMbeTargetInfo>      RTEMSTargetInfo<SparcV8TargetInfo>
  RTEMSTargetInfo<MipsTargetInfo> (×4)  RTEMSTargetInfo<SparcV8elTargetInfo>
```

**No RISC-V. No AArch64. No x86.** RISC-V is precisely the architecture RTEMS's own Clang
scaffolding targets, and it does not get `RTEMSTargetInfo` — so `riscv64-unknown-rtems7` gets
generic `OSTargetInfo` and, I believe, no `__rtems__`. I could not verify this directly because
no Clang available here was built with the RISC-V backend.

### 2. Header search — the other 2011 change

`clang/lib/Lex/InitHeaderSearch.cpp:229` lists `Triple::RTEMS` among OSes that do **not** get a
default `/usr/include`. This is Sherrill's "RTEMS tools are never self-hosted" change, still
doing its job.

`clang/lib/Driver/ToolChains/Linux.cpp:816` is a vestige:

```cpp
    if (getTriple().getOS() == llvm::Triple::RTEMS)
      return;
```

— an early return so an RTEMS triple that somehow reaches the Linux ToolChain does not get
`/include` added.

### 3. Sanitizer link flags — inert

`CommonArgs.cpp:1596,1607`: *"There's no libpthread or librt on RTEMS & Android"*, and no
`-ldl`. These only run when linking a sanitizer runtime, which does not build for RTEMS, so they
are currently unreachable.

## The gap: `Driver.cpp` has zero RTEMS references

```
$ grep -c "Triple::RTEMS" clang/lib/Driver/Driver.cpp
0
```

The ToolChain-selection switch handles AIX, Haiku, Darwin, DragonFly, OpenBSD, NetBSD, FreeBSD,
Linux, Fuchsia, Managarm, Serenity, Solaris, CUDA, AMDHSA, AMDPAL, Mesa3D, UEFI, Win32 — and not
RTEMS. There are **49 ToolChain implementations**; none is RTEMS.

The observable consequence, which I ran:

```
$ clang --target=arm-unknown-rtems7 -### hello.c     # last line
 "/usr/bin/gcc" "-o" "a.out" "/var/folders/.../rt-53be83.o"
```

The **macOS host compiler** is being handed an ARM object. Compilation also warns
`unknown platform, assuming -mfloat-abi=soft`.

## Sizing a ToolChain

| template | .cpp | .h |
|---|---:|---:|
| `BareMetal` | 674 | 126 |
| `Fuchsia` | 510 | 125 |
| `Haiku` | 289 | 79 |

`Haiku` is the realistic floor for a new OS; `BareMetal` is the closest behavioural match but is
the wrong *model* — RTEMS is a hosted POSIX RTOS with threads, a filesystem, a monotonic clock
and wide characters, and `BareMetal` assumes none of those.

An `RTEMS.cpp` would need to reproduce what `-qrtems` does in GCC (see
[02](02-rtems-in-the-gnu-toolchain.md)): start files `crti crtbegin … crtend crtn`,
`-T linkcmds`, and the `--start-group -lrtemsbsp -lrtemscpu -latomic -lc -lgcc --end-group`
group. It must also return `"rtems"` from `getOSLibName()` so runtime-library paths line up with
what compiler-rt would install.

## Runtime libraries

This section comes from a dedicated code survey rather than from things I ran; treat the file
references as reliable and the effort estimates as informed guesses.

**The runtime libraries are closer to RTEMS-ready than the driver is**, because most threading
and libc dispatch is *feature*- or *macro*-gated rather than OS-name-gated: `__thread/support.h`
has no OS gating at all, `libunwind`'s `RWMutex.hpp` is pure POSIX, and `chrono.cpp` keys on
`_POSIX_TIMERS`. The blockers are concentrated in a few hardcoded OS allowlists.

**The single highest-value change in the whole assessment** is one line in
`libcxx/include/__config` (~line 284): adding `defined(__rtems__)` to the pthread-threading
allowlist, which today `#error`s with "No thread API". That list already contains `__NuttX__`,
another RTOS. Because `libcxxabi` includes libc++'s `<__thread/support.h>` rather than having its
own abstraction, that one `#error` gates **the entire C++ runtime stack**, not just libc++. The
same edit is needed in the duplicated C++03 tree (`libcxx/include/__cxx03/__config`).

Other findings worth recording:

- **`compiler-rt/lib/builtins/clear_cache.c` will silently `abort()` on ARM32 and RISC-V RTEMS.**
  The fallback is `compilerrt_abort()`, not a compile error. Needs an `__rtems__` branch calling
  `rtems_cache_invalidate_multiple_instruction_lines`. ~15 lines, and the worst failure class
  here because it is silent.
- **`crtbegin`/`crtend` are hard-blocked** by a two-entry allowlist in
  `compiler-rt/cmake/crt-config-ix.cmake:53` (`OS_NAME MATCHES "Linux|SerenityOS"`), with no
  bare-metal escape hatch.
- **`RUNTIMES_USE_LIBC=newlib` is already first-class** — honoured at 17 header/source sites in
  libc++. RTEMS uses newlib, so a substantial slice of locale/`ELAST`/stream porting is done.
- **libunwind's open question** is unwind-info discovery, not threading: RTEMS falls into the
  `#else` branch requiring `dl_iterate_phdr`. Whether RTEMS provides it is the one item that
  could turn out to be more than small.
- Every non-builtins compiler-rt component (sanitizers, profile, XRay) is behind an OS allowlist
  that RTEMS matches none of, and these **fail silently** — an unknown OS just evaluates false.

## Work items

### Tier A — minimum viable: Clang as a drop-in for GCC

Goal: `clang --target=<arch>-rtems7 -qrtems` compiles *and links* a real RTEMS application,
using GNU binutils as assembler and linker and the RTEMS-built newlib.

| # | Item | Size |
|---|---|---|
| A1 | `clang/lib/Driver/ToolChains/RTEMS.{cpp,h}` — start files, `linkcmds`, the library group, `getOSLibName()` → `"rtems"` | **~300–500 lines**, Haiku-sized |
| A2 | `case llvm::Triple::RTEMS:` in `Driver.cpp`'s selection switch | **3 lines** |
| A3 | Accept `-qrtems` / `-qnolinkcmds` as driver options | **small** |
| A4 | Extend `RTEMSTargetInfo` wiring in `Targets.cpp` to RISC-V, AArch64, x86 | **~10 lines per arch** |
| A5 | Per-arch ABI-flag equivalence (`-mcpu=leon3`, RISC-V `-march`/`-mabi`) | **testing, not code** |

This tier needs **no** compiler-rt, no lld, no libc++. It is the smallest thing that could
plausibly be upstreamed and is worth doing on its own.

### Tier B — LLVM's own runtimes

| # | Item | Size |
|---|---|---|
| B1 | `libcxx/include/__config` + `__cxx03/__config` pthread allowlist | **2 lines** — unblocks libc++ *and* libc++abi |
| B2 | `clear_cache.c` RTEMS branch | **~15 lines**, correctness-critical |
| B3 | `crt-config-ix.cmake` + profile allowlists | **~3 one-word edits** |
| B4 | `libcxx/src/atomic.cpp` RTEMS backend at the in-tree `// <- Add other operating systems here` | small–medium |
| B5 | libunwind `dl_iterate_phdr` question | **unresolved** — small if RTEMS provides it |
| B6 | Runtimes CMake cache modelled on `Fuchsia-stage2.cmake`, `RUNTIMES_USE_LIBC=newlib`, threads ON | ~60 lines |
| B7 | Reconcile `COMPILER_RT_OS_DIR` with `getOSLibName()`; `OS_NAME` aliasing is triplicated across three cmake files | small |

Tier B total is on the order of **800–1200 lines plus build config**, excluding sanitizers — but
the shape is lopsided: a surprising amount is one-line allowlist edits, and the real engineering
sits in `clear_cache.c`, `atomic.cpp`, and the libunwind question.

### Not in scope

Sanitizers (~20 separate OS allowlists), and `lld` — which needs no RTEMS work at all, for the
same reason binutils needs almost none: RTEMS is ELF.

## How this compares to the GNU side

| | GNU | LLVM |
|---|---|---|
| OS-level compiler work | ~300 lines, written once | ~500 lines (a ToolChain), not yet written |
| per-architecture | 35–80 lines | ~10 lines, for the 8 arches LLVM has backends for |
| assembler/linker | ~30 lines of config, 2 behavioural | none needed — `lld` is ELF-generic |
| C library | newlib, ~8,600 lines (mostly imported headers) | **reuse newlib** — already first-class in the runtimes build |
| C++ runtime | libstdc++, ~40 lines of configure | libc++, gated behind a 1-line allowlist |

The asymmetry is instructive. GCC's RTEMS support is *large in aggregate but tiny per decision* —
a few hundred lines of specs written fifteen years ago that every architecture then inherits.
LLVM's missing piece is a single concentrated component, the driver ToolChain, which does not
exist at all.

## Architecture coverage is a real constraint

RTEMS 7 supports 13 architectures. Upstream LLVM has backends for **9** of them: aarch64, arm,
i386, m68k, mips, powerpc, riscv, sparc, x86_64.

Missing: **microblaze** (removed from LLVM in 2013; restoration is in flight in a sibling
project), **moxie**, **nios2**, **or1k**.

So even a complete LLVM RTEMS port could not replace GCC for the whole of RTEMS. It could cover
the architectures that matter most in practice — arm, aarch64, riscv, x86_64, sparc — which is
where RTEMS's own Clang scaffolding already points.

## Recommended order

1. **A1+A2** — the ToolChain and its one-line registration. Everything else depends on it, and it
   is independently useful because RTEMS's build system can already select Clang.
2. **A4 for RISC-V** — RTEMS's clang scaffolding targets RISC-V and `RTEMSTargetInfo` does not
   cover it. Small, and it unblocks the architecture with the most existing RTEMS-side work.
3. **B1** — two lines, unblocks the entire C++ stack.
4. **B2** — before anyone trusts the result on ARM or RISC-V, because the current failure is a
   silent `abort()`.
5. Everything else as needed.

## Verification status

Verified by running or reading directly: everything in "What exists today", the `Driver.cpp`
gap, ToolChain line counts, architecture coverage, and the `-###` link behaviour.

Not verified: the runtime-library file references come from a code survey I did not independently
re-check line by line; the RISC-V `__rtems__` claim could not be tested because no locally
available Clang has the RISC-V backend; and no part of Tier A or B has been prototyped, so the
size estimates are judgement, not measurement.

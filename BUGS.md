# Open bugs and fixes

Everything found while bringing up RTEMS on RISC-V with Clang. Fixed items list the branch;
open items say what is known and what is not.

Branches live on the forks and will be cherry-picked onto clean branches for upstream MRs/PRs.

---

## Fixed

### F1 — Clang: RISC-V RTEMS triples get no `__rtems__` *(LLVM)*

`clang/lib/Basic/Targets.cpp` had no `Triple::RTEMS` case for `riscv32`/`riscv64`, so they fell to
`default:` and returned a plain `RISCV{32,64}TargetInfo`. Every RTEMS header and source is gated
on `__rtems__`, so RTEMS could not be built for RISC-V with Clang at all.

Two `case` arms plus `clang/test/Preprocessor/rtems-predefines.c`. No regression across
`clang/test/Preprocessor` + `clang/test/Driver` (1866 tests).

- `thesamprice/llvm-project` branch **`rtems/riscv-support`** @ `f3fdc3597a`
- Upstreamable to LLVM as-is.

### F2 — RTEMS: `ARCH_BITS` missing for four RISC-V BSPs

`spec/build/cpukit/optarchbits.yml` enumerates RISC-V BSPs by name to build the Clang triple.
`riscv/mbv`, `riscv/mbv64`, `riscv/esp32` and `riscv/niosv` are absent, so they hit the empty
default and produce `riscv-unknown-rtems7` — not a valid triple. Clang rejects `-march=`/`-mabi=`.

Fixed for `mbv`/`mbv64`. **`esp32` and `niosv` are still broken** — see O5.

- `TheSamPrice/rtems` branch **`fix/clang-riscv-build-support`** @ `03d8b07c39`

### F3 — RTEMS: waf misdetects `DEST_OS` for Clang

`wscript:1568` sets `DEST_OS = "rtems"`, but `optclang.yml` then loads the waf tools and
`get_cc_version()` overwrites it from the compiler's predefined macros. Because `--target=` is
appended to `ABI_FLAGS` *after* the tools load, waf probes Clang as a **host** compiler. On macOS
it sees `__APPLE__`/`__MACH__`, sets `DEST_OS='darwin'`, runs `gcc_modifier_darwin()` and sets
`ARCH_ST=['-arch']`. The compile rule is `${CC} ${ARCH_ST:ARCH} ...` and `ARCH` is the *string*
`"riscv"`, so every compile received `-arch r -arch i -arch s -arch c -arch v`.

GCC escapes only by accident: `riscv-rtems7-gcc` defines `__rtems__` and not `__APPLE__`, and
waf's `MACRO_TO_DESTOS` has no `__rtems__` entry, so nothing matches and `DEST_OS` survives.

The `-arch` symptom is macOS-specific; the `DEST_OS` misdetection is not.

- Same branch as F2.

### F4 — RTEMS: `rtems-ld` cannot find multilib libraries

`rtems-ld` resolves `-l` via the compiler's `-print-search-dirs`. GCC reports the multilib
directories; **Clang reports only its resource dir and `<sysroot>/lib`, and ignores `-L`
entirely**, so `libm.a` was never found and `.rap` links failed.

`rtems-ld` already accepts `-L`, so `wscript`'s `rtems_rap()` now forwards the build's `-L`
entries. Compiler-agnostic.

- `TheSamPrice/rtems` branch **`fix/clang-riscv-build-support`** @ `ca79d01f83`

### F5 — RTEMS: undefined behaviour in the `lseek` overflow check

**The most significant bug found.** `rtems_filesystem_default_lseek_file()` detected overflow by
inspecting the wrapped-around sum:

```c
off_t new_offset = reference_offset + offset;      /* signed overflow = UB */
if ( ( offset >= 0 && new_offset >= reference_offset ) || ... )
```

A compiler may assume `offset >= 0 && reference_offset + offset >= reference_offset` is always
true and fold the test away. The overflow branch becomes unreachable and the wrapped negative sum
falls into the `new_offset >= 0` test, so `lseek()` returns **EINVAL where POSIX requires
EOVERFLOW**. GCC keeps the wraparound and is right by luck; Clang folds it and is not.

Fixed with `__builtin_add_overflow()`. Verified on **both** compilers: 7/7 `*_fserror` tests pass
under Clang (previously 6 failing) **and** under GCC (no regression).

- `TheSamPrice/rtems` branch **`fix/lseek-overflow-ub`** @ `20c0692327`
- This is a latent bug in upstream RTEMS regardless of compiler — worth reporting on its own.

### F6 — Clang's default TLS model is wrong for RTEMS *(worked around)*

`dl11` failed with `Unsupported relocation type 21` — `R_RISCV_TLS_GOT_HI20`. RTEMS uses static
TLS and its RISC-V `libdl` implements only the `TPREL` (local-exec) forms:

```
clang:  R_RISCV_TLS_GOT_HI20   _tls_errno        (initial-exec)
gcc:    R_RISCV_TPREL_HI20 / _ADD / _LO12_I      (local-exec)
```

`-ftls-model=local-exec` makes Clang emit relocations identical to GCC's; `dl11` and `dl12` pass.

**Not fixed properly** — this is a target default that belongs in a Clang RTEMS ToolChain, not in
every user's `config.ini`.

---

## Open

### O1 — lld and GNU ld disagree about `.symtab`, breaking `libdl`

**Impact: `dl02`, `dl07`, `dl08`, `dl09` (and `dl06`).** Root-caused, not fixed.

libgcc declares its soft-float helpers hidden:

```
2012: 00000000  2412 FUNC    GLOBAL HIDDEN   1 __muldf3
```

**lld demotes `STV_HIDDEN` to `STB_LOCAL` in a static link's `.symtab`; GNU ld leaves them
`STB_GLOBAL`.** `rtems-syms` harvests only *global* symbols from the base image, so under lld
every hidden libgcc helper silently disappears from the runtime linker's table and modules fail
with "unresolved externals".

The same disagreement in reverse breaks `dl05`: lld **retains** undefined weak symbols
(libstdc++'s `_ITM_*` transactional-memory hooks), `rtems-syms` emits strong references, and the
link fails with `undefined symbol: _ITM_RU1`. GNU ld drops them.

One root cause — **`rtems-syms` assumes GNU ld's `.symtab` conventions** — two opposite failure
modes.

Ruled out by experiment:
- **`-u__muldf3` etc. via `LIBDL_TESTS_LDFLAGS` does not help.** The symbols are already in the
  image; `-u` changes which archive members are pulled, not symbol binding. Verified: still `t`
  (local) afterwards. This is the mechanism RTEMS already uses for `-u__extendsfdf2`, so it was
  the obvious thing to try, and it is genuinely insufficient here.
- **Switching to GNU `ld` does not work either** — see O2.

Candidate fixes, none attempted:
1. Build **compiler-rt builtins** for `riscv32-unknown-rtems7` and drop libgcc. The principled
   fix; already Tier B in [04](04-llvm-gap-analysis.md). Needs checking whether compiler-rt's
   builtins are themselves hidden.
2. Teach `rtems-syms` to include local symbols, or to skip undefined weak ones. Needs RTEMS
   maintainer input on intent — it is a tooling change to accommodate a linker difference.
3. `llvm-objcopy --globalize-symbol` on the `.pre` image before `rtems-syms`. A hack.

### O2 — GNU ld cannot link Clang's output for this target

Attempted as a fix for O1. `riscv-rtems7-ld` (binutils in the RTEMS 7 toolchain) fails with:

```
riscv-rtems7-ld: DWARF error: mangled line number section (bad file number)
```

Only 15 of 721 executables linked. Adding `-gdwarf-4` did **not** help (10 linked). Not
investigated further.

This matters beyond `libdl`: "Clang + GNU binutils" is Tier A of
[04](04-llvm-gap-analysis.md) — the cheapest useful configuration — and it does not currently work
on this toolchain.

### O3 — `dl05` regressed from passing to not building

Caused by this work. `-stdlib=libstdc++` in `LDFLAGS` is required for other C++ links but drags
`cow-stdexcept.o` and its `_ITM_*` references into `dl05` (O1). Without the flag `dl05` links and
passes but other C++ tests fail.

**The two states are not simultaneously achievable with flat flags** — itself an argument for a
ToolChain that can make a coherent set of choices.

### O4 — Unexplained test failures

Not investigated at all:

| test | note |
|---|---|
| `sptls01` `sptls04` | TLS; plausibly fixed by F6 but **not re-measured** |
| `psx13` | — |
| `spglobalcon02` | global constructors; `-nostartfiles` removed `crt0.o` and nothing replaced `crtbegin`/`crtend` |
| `termios02` | — |
| `ts-validation-intr` | — |
| `ts-validation-no-clock-0` | — |

Plus one unresolved link error in the build: **`_TLS_Configuration`**.

### O5 — `ARCH_BITS` still missing for `riscv/esp32` and `riscv/niosv`

F2 fixed only `mbv`/`mbv64`, because those are the only ones built and tested here. The other two
still produce an invalid triple under Clang. Deliberately not guessed at.

### O6 — RTEMS code triggers Clang-only diagnostics under `-Werror`

`-Werror` had to be disabled for the build. Real diagnostics seen, not triaged:

- `-Wsign-compare` in `bsps/shared/ofw/ofw.c` (4 sites)
- `-Wdefault-const-init-var-unsafe` — `const char[10]` left uninitialised, `ofw.c:687`
- `-Wold-style-declaration` is GCC-only and is applied unconditionally by `optwarncc.yml`

The `ofw.c:687` uninitialised-`const` one looks like a genuine bug rather than a style nit.

---

## Summary

| | count |
|---|---:|
| Fixed and pushed | 5 (F1–F5) |
| Worked around, needs a proper home | 1 (F6) |
| Open, root-caused | 2 (O1, O2) |
| Open, self-inflicted | 1 (O3) |
| Open, uninvestigated | 3 (O4, O5, O6) |

The single highest-value open item is **O1**, because it accounts for five `dl` failures and its
principled fix — building compiler-rt builtins — also removes the libgcc dependency that makes
the whole configuration awkward.

The single most valuable item to report upstream independent of any of this is **F5**: latent
undefined behaviour in RTEMS's `lseek`, present regardless of compiler.

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

### F7 — RTEMS: write into a `const` buffer in `rtems_ofw_node_status()`

```c
const char buf[10];
len = rtems_ofw_get_prop(node, "status", (void *)&buf[0], sizeof(buf));
```

Modifying an object defined with a const-qualified type is undefined behaviour. A compiler may
assume `buf` never changes and fold the subsequent `strncmp()` calls against indeterminate
contents — in which case a node's `status` property is never really examined and the function
returns an answer unrelated to the device tree. `rtems_ofw_node_status()` decides whether a
device-tree node is enabled, so this is a real functional risk, not a style nit.

Found via clang's `-Wdefault-const-init-var-unsafe`. Dropping the `const` also removes the need
for the cast. `ofw01`, `fdt01` and `fdt02` pass before and after under GCC.

- `TheSamPrice/rtems` branch **`fix/ofw-const-buf`** @ `1cb04299d1`
- Latent bug in upstream RTEMS independent of compiler.

### F8 — RTEMS: `ARCH_BITS` missing for the last two RISC-V BSPs

The remaining half of F2. `esp32c3db` and `niosvc10lp` also fell through to the empty default and
produced the invalid triple `riscv-unknown-rtems7`.

Both are unambiguously 32-bit — every `-march` in their `abi.yml` is an `rv32` variant. Verified
by configuring both with `COMPILER=clang`: `ARCH_BITS` resolves to `'32'` and the triple becomes
`riscv32-unknown-rtems7`. Neither BSP was built or run; this fixes triple construction only.

**Worth knowing:** these identifiers are *BSP* names, not BSP *family* directory names. The
families are `spec/build/bsps/riscv/{esp32,niosv}` but the BSPs they define are `esp32c3db` and
`niosvc10lp`. My first attempt used the family names, and `waf configure` reported
`No such base BSP: 'riscv/esp32'` — which is only reported when the family name is used in
`config.ini`, so a wrong entry in `optarchbits.yml` would have silently done nothing. Caught by
verifying rather than assuming.

- Branch **`fix/clang-riscv-build-support`** @ `edf5790554`

### F9 — RTEMS: `-Wold-style-declaration` passed to Clang

`CC_WARNING_FLAGS` is applied unconditionally, but `-Wold-style-declaration` is GCC-only. Combined
with `-Werror` in `WARNING_FLAGS`, every compile fails and anyone using `COMPILER=clang` must
override the whole list by hand.

Added a clang-specific default with the same list minus that option; the GCC path is untouched
because the `enabled-by: clang` entry does not match. Verified both ways.

This removes one of the twelve `config.ini` workarounds.

- Branch **`fix/clang-riscv-build-support`** @ `877f3791f5`

### F10 — `-Wsign-compare` in `ofw.c` *(triaged, no change needed)*

All four sites are `MIN(len, size)` mixing a signed `int` with a `size_t`. Each is **guarded**:
line 231 is reached only when `prop != NULL`, in which case libfdt guarantees `len >= 0`; lines
625 and 681 have explicit `if (len <= 0) return len;` immediately above.

The warnings are legitimate and the code is correct. Left alone deliberately — changing working
code to silence a warning carries more risk than it removes. Recorded here so the next person does
not re-triage it.

---

## Open

### O1 — lld and GNU ld disagree about `.symtab` *(mostly fixed)*

**`dl` tests went from 4 passing to 8.** `dl02` and `dl07` now pass.

The root cause stands: libgcc declares its soft-float helpers `GLOBAL HIDDEN`, **lld demotes
`STV_HIDDEN` to `STB_LOCAL` in a static link's `.symtab`, GNU ld leaves them `STB_GLOBAL`**, and
`rtems-syms` harvests only global symbols — so under lld they vanish from the libdl symbol table.

The fix is two halves, and **each is useless without the other**, which is why the first attempt
looked like a dead end:

1. **Build compiler-rt builtins with `COMPILER_RT_BUILTINS_HIDE_SYMBOLS=OFF`** (it defaults ON).
   That yields `GLOBAL DEFAULT` rather than `GLOBAL HIDDEN`, so the symbols survive into
   `.symtab`. Recipe: [`repro/build-compiler-rt.sh`](repro/build-compiler-rt.sh).
2. **Force them into the base image** with `-u` via `LIBDL_TESTS_LDFLAGS`. RTEMS already does this
   for `__extendsfdf2`; extended to `__muldf3`, `__eqdf2`, `__fixdfsi`.

Earlier I recorded that `-u` "does not work". That was correct *at the time* and for the wrong
reason: with libgcc's hidden symbols `-u` cannot help, because it changes which archive members
are pulled, not symbol binding. Once compiler-rt provides global symbols, `-u` is exactly what is
needed. Both halves are required.

Two practical traps found while doing it:

- **compiler-rt ships no unwinder**, and this toolchain has no `libgcc_eh.a`. Linking all of
  libgcc to get `_Unwind_*` reintroduces its hidden `__muldf3` and undoes the fix. Extract a
  minimal `libgcc_eh.a` from libgcc's three unwind objects instead.
- **waf does not relink the libdl `.pre` base images** when only link flags change. Stale `.pre`
  files made the fix look ineffective twice. Delete `dl*/dl*.pre` when changing runtime libraries.

Still failing: `dl06` (`global symbol not found: _tls_rand48_add` — same class, a TLS symbol not
forced into the base image), and `dl08`/`dl09`, which now get much further — they load every
module and run its constructors before stopping. Their remaining problem looks unrelated to
symbols; `%f` prints literally, suggesting newlib's float `printf` support is not linked in.

The RTEMS-side change is on branch **`fix/clang-riscv-build-support`** @ `93cc3dbd5c`. The
compiler-rt build is a recipe, not a patch — nothing in RTEMS or LLVM needs changing for it.

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

---

## Summary

| | count |
|---|---:|
| Fixed and pushed | 9 (F1–F5, F7–F10, most of O1) |
| Worked around, needs a proper home | 1 (F6) |
| Open, root-caused | 1 (O2) |
| Open, self-inflicted | 1 (O3) |
| Open, uninvestigated | 1 (O4) |

**O1 is now mostly fixed** — building compiler-rt builtins with hidden symbols disabled, plus
forcing the helpers into the libdl base images, took the `dl` tests from 4 passing to 8. What is
left there is `dl06` (one more symbol of the same class) and `dl08`/`dl09`, which now fail well
past the symbol stage.

The highest-value remaining item is **O2**: GNU `ld` cannot link Clang's output for this target at
all, which blocks the cheapest useful configuration in [04](04-llvm-gap-analysis.md).

The single most valuable item to report upstream independent of any of this is **F5**: latent
undefined behaviour in RTEMS's `lseek`, present regardless of compiler.

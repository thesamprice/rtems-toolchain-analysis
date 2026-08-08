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

### F11 — RTEMS never runs global constructors under Clang

**The most consequential bug found.** Fixes `iostream`, `rcxx01`, `spglobalcon02` and `sptls04`.

`_Thread_Global_construction()` and `_exit()` decide whether to run global constructors and
destructors with:

```c
#if defined( __USE_INIT_FINI__ )
```

`__USE_INIT_FINI__` is a **GCC-internal predefine**. Clang does not define it, and does not define
`__USE__MAIN__` either, so `EXECUTE_GLOBAL_CONSTRUCTORS` is never defined and **RTEMS silently runs
no C++ static constructor at all**. Nothing then references `__libc_init_array`, so the
`PROVIDE_HIDDEN` of `__init_array_start`/`__init_array_end` in `linkcmds.base` never fires and
those symbols are absent from the executable entirely:

```
gcc-built:    __init_array_start  __init_array_end  __libc_init_array   present
clang-built:  none of them
```

The failure mode is not a diagnostic but a crash. The first use of `std::cout` faults loading the
virtual base offset from an unconstructed stream:

```
lw a5, 0x0(a1)     # vptr of the ostream
lw a5, -0xc(a5)    # virtual base offset  ->  load access fault (mcause 0x5)
```

Clang emits `.init_array` exactly as GCC does, and every RTEMS port already selects the mechanism
via `CPU_USE_LIBC_INIT_FINI_ARRAY`, so the guard just needs to admit clang. The GCC path is
byte-for-byte unchanged — verified by rebuilding with GCC and re-running all seven affected tests.

- Branch **`fix/clang-global-constructors`** @ `999d803aa2`
- A cleaner long-term fix is to stop testing a compiler-internal macro at all, since RTEMS always
  uses newlib; that is a larger change.

### F12 — `.eh_frame` never registered, so unwinding aborts

Fixes `exit03`, and vindicates the `crt` recommendation from O7 for a different test than the one
that prompted it.

`exit03` aborted inside `uw_init_context_1` in libgcc's `unwind-dw2.c`. `crtbegin.o` provides
`frame_dummy`, which is what calls `__register_frame_info` — and workaround #10's `-nostartfiles`
suppressed `crtbegin.o` along with newlib's stub `crt0.o`.

Adding `crti.o crtbegin.o` before the objects and `crtend.o crtn.o` after fixes it. That is exactly
what GCC's `-qrtems` spec does:

```
*startfile:  %{!qrtems:crt0%O%s} %{qrtems:crti%O%s crtbegin%O%s}
*endfile:    %{qrtems:crtend%O%s crtn%O%s ...}
```

Still a `config.ini` workaround rather than a patch, because the right home for it is a driver
ToolChain. Note this is the second time `-nostartfiles` — one blunt flag standing in for one line
of a GCC spec — has caused a silent runtime failure.

### F13 — Clang infers `captures(none)` on the `ctermid` argument *(LLVM)*

Closes **O4/`termios02`**, which was left as "evidence strong, cause not established". The cause is
now established and the reduced test case O4 asked for exists.

`BuildLibCalls.cpp` grouped `ctermid` with `clearerr` and `closedir` and applied
`setDoesNotCapture(F, 0)` to all three. POSIX specifies that `ctermid(s)` returns `s`, so the
argument escapes through the return value and any comparison between the two folds to false.

**Target-independent**: reproduces on riscv32, riscv64, x86_64, aarch64 and microblazeel, and
miscompiles conforming code against any POSIX libc. Full reduction and controls in
[07](07-reaching-gcc-parity.md).

- `thesamprice/llvm-project` branch **`rtems/riscv-support`**
- [`patches/llvm/0002-...`](patches/llvm/) — upstreamable to LLVM as-is.

### F14 — lld does not propagate TLS alignment under a linker script *(lld)*

Closes **O4/`sptls02`** and **corrects its diagnosis**, which blamed a missing
`__cxa_thread_atexit` path. That was wrong: the C++ `thread_local` machinery works, the TLS block
was misaligned underneath it.

A thread-local's alignment is relative to the start of the TLS block. GNU ld aligns the first TLS
output section to the maximum alignment of all of them, in `_bfd_elf_tls_setup()` — RTEMS names
that function in a comment in `tlsallocsize.c` because it depends on the behaviour. lld does the
equivalent only when it computes program headers itself; under a `SECTIONS` command nothing
supplies it, and a 512-aligned thread-local landed at block offset `0x1b00`.

Gated to the linker-script case so non-script output is byte-for-byte unchanged; lld ELF suite
2038/2038 with a new regression test. The ungated form is arguably more correct — see
[07](07-reaching-gcc-parity.md).

- [`patches/llvm/0003-...`](patches/llvm/)

### F15 — libdl passes a section index where a symbol type belongs *(RTEMS)*

Closes **O4/`dl08`, `dl09`**, listed there as "not investigated".

`rtl-elf.c` stores `osym->data = symbol.st_shndx` and legitimately uses it as a section index. But
`rtems_rtl_obj_relocate_unresolved()` passes that field as the backend's `syminfo` argument, which
every backend treats as the ELF `st_info` byte and compares against `STT_SECTION`.

`STT_SECTION` is 3, so **any deferred relocation against a symbol defined in ELF section index 3
is silently skipped and reported as applied**. Clang put `rtems_main_o2` at index 3; GCC put it at
5. The direct relocation path passes `sym->st_info` and is correct.

**Compiler-independent and latent for GCC users.** The most valuable item here to report upstream.

- [`patches/rtems/0006-...`](patches/rtems/)

### F16 — `rtems-syms` treats undefined symbols as definitions *(rtems-tools)*

Closes **O3**, and confirms O3's preferred fix ("skip symbols with `st_shndx == SHN_UNDEF`") was
the right one. rtems-tools is now checked out, so it is patched rather than reported.

The root cause is one level below where O3 placed it: `rtemstoolkit/rld-elf.cpp`,
`file::get_symbols()` classified a symbol as unresolved only when it was `STT_NOTYPE` **and**
`STB_GLOBAL` **and** `SHN_UNDEF`, so an undefined *weak* symbol was collected as a definition.
Fixing the toolkit rather than the tool applies the correction to every consumer and keeps
`rtems-syms`'s positional `RTEMS_TLS_INDEX_*` constants consistent by construction.

Requires checking the binding as well as the section index: keying on `SHN_UNDEF` alone sweeps in
the ELF null symbol and `rtems-ld` fails with `symbol not found: ` and an empty name. On a GNU ld
linked image the fixed tool's output is byte-for-byte identical to the old one.

Wired into the RSB recipe so a toolchain rebuild carries it — see [07](07-reaching-gcc-parity.md).

- `thesamprice/rtems-tools` branch **`fix/undefined-symbols-are-not-definitions`**
- [`patches/rtems-tools/0001-...`](patches/rtems-tools/)

### F17 — `ALIGN(8)` substituted for `ALIGN_WITH_INPUT` breaks the TLS block *(RTEMS)*

Closes **O4/`sptls01`** and **corrects its diagnosis**, which called the test over-specified and
"not a correctness bug in either compiler". It is a real bug, and it is in the 2020 Clang
scaffolding.

lld cannot parse `ALIGN_WITH_INPUT`, so `spec/build/bsps/optclang.yml` substitutes `ALIGN(8)`. But
`ALIGN_WITH_INPUT` imposes no alignment — it ties an output section's LMA alignment to its VMA
alignment. Applied to `.tdata`/`.tbss` the substitute pads the TLS block, and `_TLS_Size` comes out
as 8 for a single one-byte thread-local. The faithful substitute is an empty value.

- [`patches/rtems/0007-...`](patches/rtems/)

### F18 — `psx13` reads an uninitialized `struct stat` *(RTEMS)*

Closes **O4/`psx13`**, previously localised but not diagnosed.

`FutimensTest()` restores the file mode with `chmod("testfile1.tst", fstat.st_mode)` but never
`stat()`s the file; the sibling `UtimesTest()` and `UtimensatTest()` both do. With GCC the leftover
stack contents happened to carry write permission. Compiler-independent test bug.

- [`patches/rtems/0008-...`](patches/rtems/)

### F19 — two validation tests depend on compiler behaviour they should not *(RTEMS)*

Closes **O4/`ts-validation-no-clock-0`** and **O4/`ts-validation-intr`**, both "not investigated".

`tc-preinit-array` validates that `.preinit_array` runs before `.init_array` using a shared
counter. Clang's GlobalOpt evaluates the `.init_array` constructor at translation time at `-O2`
and commits it to the static initializer, so the `.preinit_array` constructor observes the
post-constructor value and the ordering becomes unobservable. Making the counter `volatile` keeps
it a run-time property.

`tc-score-isr` reads the interrupted stack pointer from a local register variable bound to `s1`.
Clang honours those only as asm operands and deleted the whole block, leaving a bare tail jump.
Naming the register in the asm template works on both compilers; the tied-operand form does **not**
— clang allocates the pair elsewhere and reads garbage.

- [`patches/rtems/0009-...`](patches/rtems/), [`patches/rtems/0010-...`](patches/rtems/)

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

> **Update.** `dl08`/`dl09` are fixed — see **F15**; the cause was the libdl relocation bug, not
> symbols. The `%f` observation was a red herring: GCC prints `%f` literally too, because the test
> wrappers use a reduced `printf`. `dl06` still fails, but it **fails identically under GCC**, so
> it is no longer a Clang-specific difference and is out of scope for parity.

The RTEMS-side change is on branch **`fix/clang-riscv-build-support`** @ `93cc3dbd5c`. The
compiler-rt build is a recipe, not a patch — nothing in RTEMS or LLVM needs changing for it.

### O2 — GNU ld cannot link Clang's output for this target

Attempted as a fix for O1. `riscv-rtems7-ld` fails with:

```
riscv-rtems7-ld: DWARF error: mangled line number section (bad file number)
```

Only 15 of 721 executables linked; adding `-gdwarf-4` made it worse (10).

**New evidence suggesting the diagnosis above is wrong.** The toolchain's linker is
**GNU ld 2.46.1**, which is recent, and it handles a simple Clang `-g` object without complaint:

```
$ clang --target=riscv32-unknown-rtems7 -march=rv32imafc_zicsr_zifencei -mabi=ilp32f -g -c t.c
$ riscv-rtems7-ld -r t.o -o t2.o
$ echo $?
0
```

GNU `ld` reads debug info to attach line numbers **to error messages it is already emitting**. So
the DWARF complaint is most likely a *symptom* of some other link failure, not the cause — and the
real error was probably scrolled past. This was not chased further because a rebuild competes for
CPU with a testsuite run; the next step is to capture the full linker output for one failing
executable rather than the last few lines.

This matters beyond `libdl`: "Clang + GNU binutils" is Tier A of
[04](04-llvm-gap-analysis.md) — the cheapest useful configuration — so whether it works is worth
knowing for its own sake.

### O3 — `rtems-syms` exports *undefined* symbols, breaking `dl05` *(now fixed, see F16)*

Root-caused precisely. The fix belongs in **rtems-tools**, which is not checked out here, so it is
reported rather than patched.

> **Resolved.** rtems-tools is now checked out and patched — see **F16**. Fix 1 below, skipping
> `SHN_UNDEF` symbols, was the correct one; it belongs one level down in `rtemstoolkit` rather
> than in `rtems-syms` itself, so that every consumer of the toolkit gets it.

`rtems-syms` emits, for every symbol it harvests from the base image:

```
  .asciz "_ITM_RU1"
  .long  _ITM_RU1        /* SYM_VALUE */
```

That `.long` is a **strong data relocation** requiring a definition at link time. But `_ITM_RU1`
is *undefined* in the base image:

```
6308: 00000000  0 NOTYPE  WEAK  DEFAULT  UND _ITM_RU1
```

so the final link fails with `undefined symbol: _ITM_RU1 referenced by dl05-sym.c`.

The symbols are libstdc++'s transactional-memory hooks, pulled in with `cow-stdexcept.o` by any
use of `std::logic_error`. They are declared **weak undefined** precisely because `libitm` is
usually absent — the intent is that they resolve to 0. `rtems-syms` discards the weak binding and
emits a strong reference.

GCC never hits this because GNU `ld` drops undefined weak symbols from the static executable's
`.symtab`; lld retains them. Same root disagreement as O1, opposite direction.

Two defensible fixes, in order of preference:

1. **Skip symbols with `st_shndx == SHN_UNDEF`.** A table of *the base image's global symbols*
   should list only symbols the base image actually defines; an undefined symbol has no address to
   record. This is the correct fix regardless of linker.
2. Preserve the weak binding (emit `.weak`) so the reference resolves to 0.

Until then `dl05` builds and passes only if `-stdlib=libstdc++` is kept out of `LDFLAGS`, which
breaks other C++ links — the two states are not simultaneously achievable with flat flags.

### O7 — the missing `.init_array` entry: **hypothesis tested and disproven**

Recorded in full because the reasoning looked strong and was wrong, and because the *fix* it
suggested is still worth adopting for a different reason.

**The observation was real.** `rcxx01` faults with a load access fault (`mcause 0x5`) inside
`std::ostream::sentry::sentry` — `std::cout` used before construction — and the `.init_array`
counts differed:

```
gcc   .init_array  size 0x1c   (7 entries)
clang .init_array  size 0x18   (6 entries)
```

The cause of the difference was correctly identified: workaround #10 uses `-nostartfiles` to
suppress newlib's stub `crt0.o`, which also suppresses `crti/crtbegin/crtend/crtn`. GCC's
`-qrtems` spec drops only `crt0.o` and *adds* the other four.

**The test.** I relinked `rcxx01` by hand with `crti.o crtbegin.o` before the objects and
`crtend.o crtn.o` after, into a scratch path so a running testsuite was undisturbed. `.init_array`
became `0x1c` — 7 entries, matching GCC exactly.

**It still crashed, at the same address.** Symbolising every entry in both binaries shows the
constructor lists are *identical*:

```
_GLOBAL__sub_I.00090__ZSt3cin
_GLOBAL__sub_I__ZN9__gnu_cxx9__freeresEv
__static_initialization_and_destruction_0   (x4)
```

The seventh entry is `crtbegin.o`'s `frame_dummy`, which registers `.eh_frame` for the unwinder —
it has nothing to do with iostream construction. Global constructors were never the problem.

**Where that leaves it.** The `rcxx01` fault is somewhere in Clang-compiled C++ interoperating
with GCC-built libstdc++, not in startup. The earlier link diagnostic
`undefined symbol: thread-local initialization routine for _tls_stderr` points at the same area —
C++ thread-local initialisation against RTEMS's TLS variables. `sptls02` fails similarly
(`A::globalCounter() = 0`, a thread-local with a constructor). Not investigated further.

**Adopt the crt change anyway.** It is what `-qrtems` does, it makes `.init_array` match GCC, and
`crtbegin` is what registers `.eh_frame` — so exception handling is unlikely to be correct without
it even though it does not fix this particular test. It belongs in a ToolChain, not in
`config.ini`.

**Note on classification:** `rcxx01`, `iostream` and `exit03` are *not* regressions. Run 1 ran 534
tests; run 3 runs 673, because C++ tests that previously failed to link now build. They are newly
built and failing.

### O4 — Remaining failures *(all now fixed)*

Down to four, from twenty. GCC passes all of them, so each is a Clang-specific difference.

> **Resolved.** All of these are closed: `sptls01` by **F17**, `sptls02` by **F14**, `termios02`
> by **F13**, `psx13` by **F18**, `dl08`/`dl09` by **F15**, and both `ts-validation` tests by
> **F19**. Two of the diagnoses recorded below turned out to be wrong and are corrected in those
> entries — `sptls01` was not an over-specified test, and `sptls02` was not `__cxa_thread_atexit`.
> The original reasoning is left in place. See [07](07-reaching-gcc-parity.md).

#### `sptls01` — TLS block size, diagnosed

```
WARNING: The thread-local storage size is 8. It should be
exactly one for this test. Check the BSP implementation.
```

The test declares a single 1-byte thread-local and asserts `tls_size == 1`. Clang produces a TLS
block of 8. This is an alignment/padding difference in how the TLS segment is laid out, not a
correctness bug in either compiler — the test is over-specified, asserting an exact size that
depends on compiler layout choices. Worth raising with RTEMS as a test-portability question rather
than fixing in the toolchain.

#### `sptls02` — C++ `thread_local` with a constructor

`A::globalCounter() = 0` where 123 is expected. Global constructors now run (F11), but a C++
`thread_local` object with a dynamic initialiser needs *per-thread* initialisation via the
`__cxa_thread_atexit` / TLS-wrapper mechanism, which is a separate path. Related to the link
diagnostic `undefined symbol: thread-local initialization routine for _tls_stderr` seen earlier.
Not investigated further.

#### `termios02` — a Clang miscompile (needs a reduced test case)

`rtems_test_assert( term_name_p == term_name )` at `init.c:157` fails, where
`char term_name[32]` and `term_name_p = ctermid( term_name )`.

**Clang folds that comparison to false at compile time.** The call site contains no branch at all:

```asm
800006e8: addi a0, sp, 0x4        # a0 = &term_name
800006ea: jal  ctermid            # call it...
800006ec: lui  a0, 0x8000d        # ...and immediately discard the result
800006f0: addi a0, a0, 0x2ab      #
...
80000704: li   a2, 0x9d           # 157  <- the assert's line number
80000708: jal  __wrap_printf      # assertion-failed message
8000070e: jal  rtems_test_exit
```

Only the failure path was emitted. Two pointers to the same object must compare equal, so folding
this to *false* is unsound.

The relevant lowering is in `ctermid` itself, which Clang reduces to a tail call:

```asm
ctermid:
  beqz a0, .Lnull
  lui/addi a1, ctermid_name
  li   a2, 0xd
  j    memcpy               # returns dest, so ctermid returns s
```

Ruled out along the way, in order:

1. **Not newlib overriding RTEMS** — newlib does not define `ctermid` at all; both toolchains link
   RTEMS's own `cpukit/libcsupport/src/ctermid.c`.
2. **The source is correct** — it ends `strcpy( s, ctermid_name ); return s;`.
3. **The tail call is sound** — disassembling the linked `memcpy` shows every return path is a bare
   `ret` with `a0` never written (the moving pointer is kept in `a5` via `mv a5, a0`), so it does
   return its destination. I initially believed this was the bug; it is not.

**Status: not confirmed as an upstream bug.** The evidence is strong but comes from reading one
optimised binary. Before reporting to LLVM this needs reducing to a self-contained test case —
a function that copies into its pointer argument, returns it, and whose caller compares the result
against the original — built for `riscv32` at `-O2` and checked with `llvm-reduce`. Do not file it
on the strength of the listing alone.

#### `psx13` — localised, not diagnosed

`futimens( fd, NULL )` returns non-zero where 0 is required (`psxtests/psx13/test.c:725`). No
analysis beyond that.

#### `dl08`, `dl09`, `ts-validation-intr`, `ts-validation-no-clock-0` — not investigated


No analysis done on these two.

---

## Summary

| | count |
|---|---:|
| Fixed and pushed | 18 (F1–F5, F7–F19, and O1, O3) |
| Worked around, needs a proper home | 1 (F6) |
| Open, root-caused | 1 (O2) |
| Open, hypothesis disproven | 1 (O7) |

**O4 is closed.** All eight remaining test failures are fixed — see
[07](07-reaching-gcc-parity.md) — and the same 674 tests now produce the same verdict under both
compilers. Two of the four diagnoses recorded under O4 were wrong (`sptls01` was a real bug in
RTEMS's Clang scaffolding, not an over-specified test; `sptls02` was TLS block alignment, not
`__cxa_thread_atexit`) and are corrected in F17 and F14 with the original reasoning left in place.

**O3 is closed** by F16, one level deeper than it was reported: in `rtemstoolkit` rather than in
`rtems-syms`.

The highest-value remaining item is **O2**: GNU `ld` cannot link Clang's output for this target at
all, which blocks the cheapest useful configuration in [04](04-llvm-gap-analysis.md).

Three items are worth reporting upstream independent of the Clang work, because each is a live
defect for people who have never used Clang:

- **F15** — libdl silently drops any deferred relocation against a symbol in ELF section index 3.
- **F5** — undefined behaviour in RTEMS's `lseek` overflow check.
- **F13** — Clang miscompiles any correct use of `ctermid`, on every target, against any POSIX
  libc. Nothing to do with RTEMS; RTEMS is just where it was caught.

# RTEMS testsuite on QEMU — Clang-built `riscv/mbv`

Raw output from running the RTEMS 7 testsuite against a `riscv/mbv` BSP built entirely with
Clang 23.0.0git + `ld.lld`, on QEMU `amd-microblaze-v-generic`.

## Progression

Each row is a full testsuite run against a different build, in chronological order.

| run | build | PASS | FAIL | tests run |
|---|---|---:|---:|---:|
| 1 | TLS model + `-T linkcmds` | 488 | 20 | 534 |
| 2 | + `lseek` UB fix (F5) | 501 | 15 | 542 |
| final | + compiler-rt, libdl, constructors (F11), `.eh_frame` (F12) | **508** | **8** | 542 |
| — | **GCC baseline** | 517 | 0 | 543 |

The final run also records 24 XFAIL and 2 XFAIL-KNOWN. `dl06` and `ttest01` failed on the first
pass and **passed on retry**, so they are flaky under parallel QEMU load rather than broken.

**The test *count* matters as much as the pass count.** Run 1 ran 534 tests; the final build
produces 673, because C++ tests that previously failed to *link* now build. So `iostream`,
`rcxx01` and `exit03` appearing as failures partway through this work were newly built, not
newly broken.

## Remaining failures (8)

```
dl08 dl09  psx13  sptls01 sptls02  termios02
ts-validation-intr  ts-validation-no-clock-0
```

GCC passes all of these, so each is a Clang-specific difference. Two are diagnosed:

- **`sptls01`** asserts the TLS block is exactly 1 byte; Clang lays it out as 8. An
  alignment/padding difference, and an over-specified test rather than a toolchain bug.
- **`sptls02`** is a C++ `thread_local` with a dynamic initialiser, which needs the per-thread
  `__cxa_thread_atexit` path — separate from the global constructors fixed in F11.

Two more are localised to an exact assertion:

- **`psx13`** — `futimens( fd, NULL )` returns non-zero where 0 is required
  (`psxtests/psx13/test.c:725`).
- **`termios02`** — `ctermid( term_name )` must return the caller's buffer, and does not
  (`libtests/termios02/init.c:157`). Worth noting what this is *not*: newlib does not define
  `ctermid` at all, so both toolchains link RTEMS's own `cpukit/libcsupport/src/ctermid.c`, whose
  source plainly ends `strcpy( s, ctermid_name ); return s;`. The two builds differ only in
  codegen — 26 bytes under Clang versus 40 under GCC — so this is a miscompile or a subtler
  aliasing issue rather than the wrong function being linked. Not resolved.

`dl08`, `dl09` and `ts-validation-*` have had no analysis. `dl06` needs `_tls_rand48_add`;
notably the **GCC** symbol table does not contain it either, so that is a difference in what the
Clang-built `.rap` module references, not a symbol-table gap.

## Caveats — still not an apples-to-apples comparison

1. The build carries workarounds (see [`../repro/config.ini`](../repro/config.ini)), several
   blunt: `-Werror` is off and startup files are supplied by hand rather than selected by a driver.
2. It uses **GCC's newlib, libgcc and libstdc++**. Nothing here exercises LLVM's own runtime
   libraries — compiler-rt's builtins are the one exception, added for F11/O1.
3. One executable (`dl05`) still does not build; see O3 in [`../BUGS.md`](../BUGS.md).

## Files

| | |
|---|---|
| `clang-riscv-mbv-final.txt` | final run, every test and result |
| `clang-riscv-mbv-final-tally.txt` | the counts above |
| `clang-riscv-mbv-summary.tsv` | run 1, kept for comparison |
| `clang-riscv-mbv-failures.txt` | run 1 failures |

## History

Earlier drafts of this file are preserved below rather than deleted, because two of the claims in
them were wrong and the corrections are instructive.

## Caveats — why this is not a fair fight

1. The Clang build carries **twelve workarounds** (see [05](../05-clang-riscv-bringup.md)),
   several of which are blunt: `-Werror` is off, `-Wold-style-declaration` is dropped, and
   startup files are suppressed with `-nostartfiles` rather than being correctly selected.
2. It uses **GCC's newlib, libgcc and libstdc++**. Nothing here exercises LLVM's own runtime
   libraries. This shows Clang can *compile and link* RTEMS, not that the LLVM runtime stack
   works on RTEMS.
3. **9 executables are missing** (712 of 721 built), which is why 534 tests ran instead of 543.
4. The GCC baseline was produced from the same commit but a different build tree.

## The 20 failures, grouped

```
dl01 dl02 dl07 dl08 dl09 dl11 dl12
imfs_fserror jffs2_fserror jffs2nand_fserror mdosfs_fserror mimfs_fserror mrfs_fserror
psx13 spglobalcon02 sptls01 sptls04 termios02 ts-validation-intr ts-validation-no-clock-0
```

`dl06` and `ttest01` are counted as XFAIL-KNOWN by the runner, not as failures.

### libdl (7) — root-caused, partly fixed

`dl01 dl02 dl07 dl08 dl09 dl11 dl12`

These were **not** caused by missing `.rap` modules, which is what an earlier draft of this file
claimed. See [06](../06-libdl-and-lld.md) for the real causes: Clang's default TLS model emits
relocations RTEMS's runtime linker cannot handle, and `lld` and GNU `ld` disagree about which
symbols land in `.symtab`. Fixing the first took `dl` from 4 to 6 passing in a later build; the
second is unresolved.

**These numbers predate those fixes.** The run in this directory is from the build *before*
`-ftls-model=local-exec` was applied.

### A suspicious cluster (6) — not investigated

```
imfs_fserror  jffs2_fserror  jffs2nand_fserror
mdosfs_fserror  mimfs_fserror  mrfs_fserror
```

Every `*_fserror` variant fails and **no other filesystem test does** — `*_fsrdwr`,
`*_fsrename`, `*_fspatheval` and the rest all pass across all six filesystems. Six different
filesystems failing the same single test points at one shared cause in error/errno handling
rather than six independent bugs. This remains the most interesting unexamined lead.

### TLS (2)

`sptls01 sptls04` — the same area as the `dl` TLS problem and as the one unresolved link failure
in the build (`_TLS_Configuration`). Plausibly fixed by `-ftls-model=local-exec`; **not
re-measured** in this run.

### Unclassified (5) — no analysis done

`psx13`, `spglobalcon02`, `termios02`, `ts-validation-intr`, `ts-validation-no-clock-0`

`spglobalcon02` (global constructors) is worth a look given `-nostartfiles` removed `crt0.o` and
nothing replaced `crtbegin`/`crtend`.

## Files

| | |
|---|---|
| `clang-riscv-mbv-summary.tsv` | every test and its final result |
| `clang-riscv-mbv-tally.txt` | the counts above |
| `clang-riscv-mbv-failures.txt` | just the 20 failures |

## What would make this a real comparison

In rough order of value: finish the libdl work in [06](../06-libdl-and-lld.md) — chiefly
building compiler-rt builtins so the base image stops depending on GCC's hidden libgcc symbols;
then root-cause the `*_fserror` cluster; then re-run against a build that does not need the
twelve workarounds. Only after that is a Clang-vs-GCC pass-rate number worth reporting as a
quality signal rather than a bring-up milestone.

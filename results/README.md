# RTEMS testsuite on QEMU — Clang-built `riscv/mbv`

Raw output from running the RTEMS 7 testsuite against a `riscv/mbv` BSP built entirely with
Clang 23.0.0git + `ld.lld`, on QEMU `amd-microblaze-v-generic`.

## Headline

| | Clang | GCC baseline |
|---|---:|---:|
| tests run | 534 | 543 |
| PASS | **488** | 517 |
| XFAIL | 24 | 24 |
| XFAIL-KNOWN | 2 | 2 |
| FAIL | 20 | 0 |

The runner retries anything that fails the first time. Five tests (`fdt01`, `fdt02`, `tar01`,
`tar02`, `psxftw01`) produced no output on the first pass and **passed on retry** — they were
starved by running four QEMU instances in parallel, not broken. An interim figure of 483 PASS /
22 FAIL / 5 NO-OUTPUT was recorded before the retry pass completed; the table above is the final
result and supersedes it.

**This is not an apples-to-apples comparison.** Read the caveats below before quoting these
numbers.

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

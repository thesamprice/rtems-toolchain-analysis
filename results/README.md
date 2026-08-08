# RTEMS testsuite on QEMU — Clang-built `riscv/mbv`

Raw output from running the RTEMS 7 testsuite against a `riscv/mbv` BSP built entirely with
Clang 23.0.0git + `ld.lld`, on QEMU `amd-microblaze-v-generic`.

## Headline

| | Clang | GCC baseline |
|---|---:|---:|
| tests run | 534 | 543 |
| PASS | **483** | 517 |
| XFAIL | 24 | 24 |
| XFAIL-KNOWN | — | 2 |
| FAIL | 22 | 0 |
| NO-OUTPUT | 5 | 0 |

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

## The 27 failures, grouped

Grouping is by inspection of the names; **no root-cause analysis has been done** on any of these.
Treat the attributions below as hypotheses, not findings.

### Explained by the incomplete build (10)

| tests | why |
|---|---|
| `dl01 dl02 dl06 dl07 dl08 dl09 dl11 dl12` | the `.rap` loadable modules failed to build — `rtems-ld` could not find the multilib `libm.a`. These tests cannot pass without them. |
| `fdt01 fdt02` | NO-OUTPUT; not investigated |

Note `dl03 dl04 dl05 dl10` *passed*, so the `dl` failures are not uniform.

### A suspicious cluster (6)

```
imfs_fserror  jffs2_fserror  jffs2nand_fserror
mdosfs_fserror  mimfs_fserror  mrfs_fserror
```

Every `*_fserror` variant fails and **no other filesystem test does** — `*_fsrdwr`,
`*_fsrename`, `*_fspatheval` and the rest all pass across all six filesystems. Six different
filesystems failing the same single test points at one shared cause in error/errno handling
rather than six independent bugs. This is the most interesting lead in the run.

### TLS (2)

`sptls01 sptls04` — plausibly related to the one unresolved link failure in the build
(`_TLS_Configuration`), which is the other known-incomplete area.

### Unclassified (9)

`psx13`, `psxftw01`, `spglobalcon02`, `tar01`, `tar02`, `termios02`, `ts-validation-intr`,
`ts-validation-no-clock-0`, `ttest01`

`spglobalcon02` (global constructors) is worth a look given `-nostartfiles` removed `crt0.o` and
nothing replaced `crtbegin`/`crtend`.

## Files

| | |
|---|---|
| `clang-riscv-mbv-firstpass.txt` | every test and its result |
| `clang-riscv-mbv-tally.txt` | the counts above |
| `clang-riscv-mbv-failures.txt` | just the 27 non-passes |

## What would make this a real comparison

In rough order of value: build the `.rap` modules so the `dl` tests can run; fix the TLS link
error; then root-cause the `*_fserror` cluster. Only after that is a Clang-vs-GCC pass-rate
number worth reporting as a quality signal rather than a bring-up milestone.

# Getting the `dl` tests working: two TLS and symbol-table incompatibilities

RTEMS's `libdl` runtime linker loads ELF objects at runtime and resolves them against a symbol
table generated from the base image by `rtems-syms`. That makes it far more sensitive to
toolchain details than ordinary code, and it surfaced three distinct problems that nothing else
in the testsuite did.

Starting point: 4 of 12 `dl` tests passing. Now: **6 of 12**, with the remaining failures traced
to a single root cause.

## First, a correction

[05](05-clang-riscv-bringup.md) and the first `results/README.md` attributed the `dl` failures to
missing `.rap` modules. **That was wrong.** The GCC baseline also builds exactly one `.rap`
(`dl06.rap`) — the other tests load plain `.o` files from a tar archive at runtime. I had labelled
it a hypothesis rather than a finding, which is the only reason it did not become a false claim,
but it was still a bad guess stated too confidently.

The `.rap` build failure was real, just not the cause of the test failures.

## Fix 1 — `rtems-ld` could not find multilib libraries *(fixed in RTEMS)*

`rtems-ld` resolves `-l` arguments by asking the target compiler for its search paths. GCC reports
the multilib directories. Clang reports only its resource directory and `<sysroot>/lib`, **and
ignores `-L` entirely**:

```
$ clang --target=riscv32-unknown-rtems7 -L<multilib> -print-search-dirs
libraries: =<resource-dir>:<sysroot>/lib
```

vs GCC, which lists `.../rv32imafc/ilp32f/` first. So `libm.a` was never found.

`rtems-ld` already accepts `-L`, so `wscript`'s `rtems_rap()` now forwards the `-L` entries the
build already has. Compiler-agnostic, and helps any toolchain whose `-print-search-dirs` is less
informative than GCC's.

## Fix 2 — Clang defaults to a TLS model RTEMS's runtime linker cannot relocate

`dl11` failed with:

```
dlopen failed: .text.get_errno_val: Unsupported relocation type 21 in non-PLT
```

Type 21 is `R_RISCV_TLS_GOT_HI20`. Comparing the same source object between toolchains:

```
clang:  R_RISCV_TLS_GOT_HI20   _tls_errno      (initial-exec)
gcc:    R_RISCV_TPREL_HI20     _tls_errno      (local-exec)
        R_RISCV_TPREL_ADD      _tls_errno
        R_RISCV_TPREL_LO12_I   _tls_errno
```

RTEMS uses static TLS and its RISC-V `libdl` backend implements only the `TPREL` forms. GCC's
RTEMS configuration defaults to local-exec; Clang does not, because nothing tells it this is a
statically-linked RTOS.

`-ftls-model=local-exec` makes Clang emit relocations identical to GCC's, and `dl11` passes.

**This belongs in a driver ToolChain**, not in every user's `config.ini` — it is exactly the kind
of target default `RTEMS.cpp` would set.

## Fix 3 — the unfixed one: lld and GNU ld disagree about `.symtab`

This is the most interesting finding, and it is **not fixed**.

`dl02` failed with "has unresolved externals". The object needed `__muldf3`, `__eqdf2` and
`__fixdfsi` — libgcc's soft-double helpers. Both base images *contain* them. The difference is the
binding:

```
clang/lld build:   80016862 t __muldf3      <- LOCAL
gcc/GNU ld build:  8001528c T __muldf3      <- GLOBAL
```

Because libgcc declares them hidden:

```
2012: 00000000  2412 FUNC    GLOBAL HIDDEN   1 __muldf3
```

**lld demotes `STV_HIDDEN` symbols to `STB_LOCAL` in the static output's `.symtab`. GNU ld leaves
them `STB_GLOBAL`.** Both are defensible readings — hidden visibility is about dynamic symbol
export, and there is no dynamic symbol table here at all — but `rtems-syms` harvests *global*
symbols from the base image, so under lld every hidden libgcc helper silently disappears from the
runtime linker's symbol table.

The same disagreement, in the opposite direction, breaks `dl05`: lld **retains** undefined weak
symbols (`_ITM_RU1`, `_ZGTtnaj`, and the rest of libstdc++'s transactional-memory hooks) in
`.symtab`, `rtems-syms` emits strong references to them, and the final link then fails with
`undefined symbol: _ITM_RU1`. GNU ld drops them, so GCC never sees this.

So one root cause — **`rtems-syms` assumes GNU ld's `.symtab` conventions** — produces two
opposite failure modes under lld.

### Options, none taken

1. **Build compiler-rt builtins for RTEMS.** compiler-rt does not mark its builtins hidden, so the
   symbols would stay global. This is the principled fix and is already listed as Tier B work in
   [04](04-llvm-gap-analysis.md) — it removes the libgcc dependency entirely rather than working
   around it.
2. **Teach `rtems-syms` to include local symbols**, or to skip undefined weak ones. Plausible, but
   it is a change to RTEMS's tooling to accommodate a linker difference, and the first option is
   better.
3. Force the symbols global with a linker script or `-Wl,--undefined`. A workaround, not a fix.

I did not attempt any of these — option 1 is a genuine piece of work and option 2 needs RTEMS
maintainer input on intent.

## Where the `dl` tests stand

Measured on the current build, run serially under QEMU:

| result | tests |
|---|---|
| **PASS (6)** | `dl01` `dl03` `dl04` `dl10` `dl11` `dl12` |
| **FAIL — hidden-symbol issue** | `dl02` `dl07` `dl08` `dl09` |
| **FAIL — TLS symbol not exported** | `dl06` (`global symbol not found: _tls_rand48_add`, same root cause) |
| **NO BUILD** | `dl05` |

Before this work: 4 passing (`dl03` `dl04` `dl05` `dl10`). Now: 6. So `dl01`, `dl11` and `dl12`
are newly passing, and `dl05` has **regressed from passing to not building**.

That regression is mine: adding `-stdlib=libstdc++` to `LDFLAGS` was needed to get other C++
links working, and it is what drags libstdc++'s `cow-stdexcept.o` — and its `_ITM_*` references —
into `dl05`. Without that flag `dl05` links and passes but other C++ tests fail to link.
**The two states are not simultaneously achievable with the current workarounds.** That is itself
evidence that this belongs in a ToolChain that can make a coherent set of choices, rather than in
a flat list of flags.

`dl06.rap` now builds, which it previously did not — but `dl06` still fails at runtime for the
symbol-table reason above.

## Honest summary

Of the three problems, two are now fixed and both fixes are real and upstreamable — one to RTEMS,
one that belongs in a Clang ToolChain. The third is a genuine `lld`-vs-GNU-`ld` behavioural
difference that no flag papers over, and the right answer is to stop depending on GCC's libgcc at
all.

# Reaching parity with GCC on the test suite

[06](06-libdl-and-lld.md) left eight tests failing under Clang that pass under GCC. This document
closes all eight and records what each one turned out to be.

The headline is that **the same 674 tests now produce the same verdict under both compilers** —
`diff` of the two result files is empty. The more useful finding is the breakdown: of the eight
root causes, **two were compiler bugs and six were not**. Four were latent defects in RTEMS and its
tools that GNU ld's habits and GCC's stack layout had been covering for, one was a defect in the
2020 Clang scaffolding, and one was self-inflicted by my own workaround.

## First: the numbers in [05](05-clang-riscv-bringup.md) and [06](06-libdl-and-lld.md) were not comparable

Before any of the fixes, the measurement had to be redone.

The earlier runs reported 508 pass / 8 fail out of 542 for Clang against 517 / 0 out of 543 for
GCC. Both numbers are real; comparing them is not meaningful. The denominators differ, the two
sides skipped different tests, and — the one that actually mattered — a number of `dl*` binaries
were stale artifacts from an older build that were not being rebuilt at all. They "passed" because
nothing had asked them the question recently. Regenerating them exposed a whole family of
failures that had been invisible.

Everything below uses one runner ([`tools/run-all.sh`](tools/run-all.sh)) that walks every `.exe`
in a build tree, runs each under an identical QEMU invocation and timeout, and classifies on
identical strings. It is run once per compiler and the two result files are diffed. That is the
only form of this claim worth making.

| | PASS | XFAIL | SKIP | FAIL | total |
|---|---:|---:|---:|---:|---:|
| GCC | 633 | 25 | 9 | 7 | 674 |
| Clang | 633 | 25 | 9 | 7 | 674 |

The seven remaining failures — `cpuuse`, `dl06`, `sp04`, `spcontext01`, `sptimecounter03`,
`tmfine01`, `ttest01` — fail identically under GCC and have not been chased. They are most likely
artifacts of the runner's 25-second timeout. **Parity means matching, not zero**, and the caveats
at the end of this document matter more than the table does.

## The two genuine compiler bugs

### 1. Clang infers `captures(none)` on the `ctermid` argument *(LLVM)*

This closes `termios02`, which [06](06-libdl-and-lld.md) left as "evidence strong, cause not
established, do not file on the strength of a listing". The listing was right and the cause is
now established.

`llvm/lib/Transforms/Utils/BuildLibCalls.cpp` grouped `ctermid` with `clearerr` and `closedir`:

```cpp
  case LibFunc_ctermid:
  case LibFunc_clearerr:
  case LibFunc_closedir:
    Changed |= setRetAndArgsNoUndef(F);
    Changed |= setDoesNotThrow(F);
    Changed |= setDoesNotCapture(F, 0);
    break;
```

`captures(none)` is correct for the other two — neither returns its pointer argument. POSIX
specifies that `ctermid(s)`, given non-null `s`, writes the pathname into `s` and **returns `s`**.
The argument escapes through the return value, so alias analysis concludes the returned pointer
cannot be the buffer, and any comparison between them folds to false.

The self-contained reduction [06](06-libdl-and-lld.md) asked for:

```c
char *ctermid(char *s);           /* exact POSIX signature */
int check_named(void) {
  char buf[16];
  char *p = ctermid(buf);
  return p == buf;                /* must be 1 */
}
```

At `-O2` this compiles to `li a0, 0`. Three controls establish the mechanism:

| variant | result |
|---|---|
| callee named `ctermid` | folds to `0` |
| same code, callee renamed | comparison preserved |
| `ctermid` with `-fno-builtin` | comparison preserved |

The rename control matters methodologically. My first attempt at isolating this stripped
`captures(none)` from the IR while leaving the callee named `ctermid`, and both variants still
folded — because `opt` re-derives the attribute from the *name* via `TargetLibraryInfo`. Renaming
the callee is the only way to separate the attribute from the recognition.

Attribute isolation, with the callee renamed so TLI cannot re-infer:

```llvm
declare ptr @notalibfunc(ptr captures(none))   ; -> ret i32 0
declare ptr @notalibfunc(ptr)                  ; -> comparison preserved
```

Necessary and sufficient. **This is target-independent** — it reproduces on riscv32, riscv64,
x86_64, aarch64 and microblazeel — and it miscompiles conforming code against any POSIX libc. It
is not an RTEMS or a RISC-V bug; RTEMS is just where it happened to be caught, by a test that
asserts precisely what POSIX promises.

The fix splits `ctermid` into its own arm, keeping `noundef` and `nounwind` and dropping the
capture attribute, and updates `llvm/test/Transforms/InferFunctionAttrs/annotate.ll`. The
neighbouring library functions that also return a pointer argument — `realpath`, `fgets`,
`stpcpy`, `strtok`, `gets` — were audited and all mark the correct argument. `ctermid` was the
lone outlier.

### 2. lld does not propagate TLS alignment under a linker script *(lld)*

This closes `sptls02`, and **corrects the diagnosis in [06](06-libdl-and-lld.md)**, which
attributed it to a missing `__cxa_thread_atexit` per-thread initialisation path. That was wrong.
The C++ `thread_local` machinery works; the TLS block was misaligned underneath it.

`sptls02` declares thread-locals with `alignas(256)` and `alignas(512)` and checks their runtime
addresses. The section layout differs:

```
clang  .tdata  PROGBITS  80029500  align 256
       .tbss   NOBITS    8002a000  align 4096
gcc    .tdata  PROGBITS  8002b000  align 4096
       .tbss   NOBITS    8002c000  align 4096
```

A thread-local's alignment is relative to the start of the TLS block, not to its own output
section. GCC's `.tdata` inherited alignment 4096 from `.tbss`; Clang's kept its own 256. The
512-aligned variable landed at block offset `0x1b00`, and `0x1b00 % 512 = 0x100`.

GNU ld does this propagation in `_bfd_elf_tls_setup()`. RTEMS depends on it explicitly — there is
a comment in `cpukit/score/src/tlsallocsize.c` that names the function:

> The linker ensures that the first TLS output section is aligned to the maximum alignment of all
> TLS output sections, see function `_bfd_elf_tls_setup()` in `bfd/elflink.c`.

lld has the equivalent effect, but only on the path where it computes the program headers itself,
by aligning the `PT_LOAD` that starts `PT_TLS` (`Writer::assignAddresses`). Under a `SECTIONS`
command that path does not run and nothing else supplies it. A minimal reproduction with a
hand-written script confirms the split: without a script lld gets it right; with one it does not.

The fix propagates the maximum TLS alignment onto the first TLS output section in
`LinkerScript::adjustOutputSections()`.

**It is deliberately gated to the linker-script case.** The ungated version is arguably more
correct and matches GNU ld everywhere, but it changes `sh_addralign` on `.tdata` in two existing
lld tests (`aarch64-tls-vaddr-align.s`, `i386-tls-vaddr-align.s`) that exist specifically to pin
the current behaviour. Addresses were identical either way — only the recorded alignment changed.
With the gate, non-script output is byte-for-byte unchanged and the lld ELF suite is 2038/2038,
including a new regression test. A reviewer may reasonably prefer the ungated form; that is a
question for review, not one to settle unilaterally.

## The four latent RTEMS bugs

None of these are Clang bugs. All four are defects that a second toolchain merely exposed.

### 3. libdl passes a section index where a symbol type belongs

This closes `dl08` and `dl09`, which [06](06-libdl-and-lld.md) listed as "not investigated". It is
the most interesting bug in this study.

Both tests load one object, then a second that satisfies the first one's undefined references.
Under Clang, `&rtems_main_o2` in the first module read as `0` and the test jumped through null.
RTL tracing showed all three deferred relocations being re-applied after the defining object
loaded, reporting success. Instrumenting the RISC-V backend showed the relocation handlers were
never reached — the function returned early.

`cpukit/libdl/rtl-elf.c` stores, when loading a symbol:

```c
osym->data = symbol.st_shndx;
```

and legitimately uses `osym->data` as a section index elsewhere (`rtems_rtl_elf_symbols_locate`).
But `rtems_rtl_obj_relocate_unresolved()` passes that same field as the `syminfo` argument to the
architecture backend — and the backends treat `syminfo` as the ELF `st_info` byte, testing it
against `STT_SECTION` to skip relocations against section symbols.

`STT_SECTION` is 3. So **any deferred relocation against a symbol defined in ELF section index 3
is silently skipped and reported as successfully applied.**

```
clang  dl09-o2.o:  80: 244 FUNC GLOBAL DEFAULT 3 rtems_main_o2   -> skipped
gcc    dl09-o2.o: 132: 262 FUNC GLOBAL DEFAULT 5 rtems_main_o2   -> applied
```

Clang's section ordering put the function at index 3; GCC's put it at 5. That is the entire
difference between working and not. The direct, non-deferred path passes `sym->st_info` and is
correct; only the deferred path is wrong. Only the RISC-V and MIPS backends read `syminfo` at all,
and both only for this one comparison.

This is a live hazard for GCC users who have never touched Clang. It will stay invisible until
someone's cross-module symbol happens to land in section index 3.

The fix passes `STT_NOTYPE` in the deferred path — a symbol on the unresolved list is always a
named symbol resolved from another object or the base image, never a section symbol.

### 4. `rtems-syms` treats undefined symbols as definitions *(rtems-tools)*

This closes **O3**, and confirms that the fix O3 identified as preferable was the right one. O3
reported it and could not patch it because rtems-tools was not checked out; it is now.

The root cause is one level deeper than O3 placed it — not in `rtems-syms` but in the shared
toolkit underneath. `rtemstoolkit/rld-elf.cpp`, `file::get_symbols()` classified a symbol as
unresolved only when it was `STT_NOTYPE` **and** `STB_GLOBAL` **and** `SHN_UNDEF`. Anything else
undefined fell through to a second test that looks only at type and binding — so an undefined
*weak* symbol was collected as a definition and handed to the tools as one.

GNU ld drops unresolved weak undefined symbols from the symbol table of a fully linked image; lld
keeps them. That is the whole exposure, and it is the same root disagreement as O1 and O3.

Fixing it in the toolkit rather than in `rtems-syms` means the correction applies to every
consumer, and it makes the table self-consistent by construction: `rtems-syms` numbers its
`RTEMS_TLS_INDEX_*` constants by walking the same already-filtered list, so nothing can drift.

One detail worth recording. My first attempt keyed only on `SHN_UNDEF`, which swept in the ELF
null symbol — local, undefined, empty name. `rtems-ld` immediately failed with:

```
error: dl06-o1.o: symbol not found:
```

with nothing after the colon. Requiring `STB_GLOBAL` or `STB_WEAK` fixes that and exactly preserves
the previous behaviour for local symbols. On a GNU ld linked image the fixed tool's output is
byte-for-byte identical to the old one.

### 5. `psx13` reads an uninitialized `struct stat`

This closes `psx13`, which [06](06-libdl-and-lld.md) had localised to `futimens(fd, NULL)`
returning non-zero without diagnosing it.

Instrumenting `futimens()` showed `rtems_filesystem_utime_check_permissions()` returning `EACCES`
— the file had no write permission. Walking back, `FutimensTest()` makes the file read-only for an
`EACCES` case and then restores the mode with:

```c
rv = chmod( "testfile1.tst", fstat.st_mode );
```

but nothing in that function ever fills `fstat` in. The sibling functions `UtimesTest()` and
`UtimensatTest()` both `stat()` the file first; `FutimensTest()` was missing it. So the restoring
`chmod()` applies whatever was on the stack.

With GCC the leftover stack contents happened to carry write permission and the test passed. This
is a compiler-independent test bug that GCC has been passing by luck.

### 6. `tc-preinit-array` lets the compiler evaluate the thing it is testing

This closes `ts-validation-no-clock-0`, listed in [06](06-libdl-and-lld.md) as "not investigated".
The failing case is:

```
F:2:0:RUN:tc-preinit-array.c:138:2 == 1
F:3:0:RUN:tc-preinit-array.c:139:1 == 2
```

The test validates that `.preinit_array` constructors run before `.init_array` ones, by having
both increment a shared counter and checking the order. The two values came out exactly swapped.

Clang's GlobalOpt evaluates the `.init_array` constructor at translation time at `-O2` and commits
its effect to the static initializer — the object file has no `.init_array` section at all, and
`llvm.global_ctors` is emptied. The `.preinit_array` constructor, which really does run first,
then observes the *post*-constructor value of the counter.

Whether that transform is sound in the presence of `.preinit_array` is a genuine grey area: LLVM
has no IR representation for a function the runtime calls before global constructors, so it cannot
know. The practical fix is to make the counter `volatile`, which keeps the sequence a run-time
property and is what a test validating run-time ordering should have done anyway.

### 7. `tc-score-isr` reads a local register variable outside an asm operand

This closes `ts-validation-intr`, also listed as "not investigated".

The RISC-V interrupt-dispatch wrapper obtains the interrupted stack pointer from `s1`:

```c
register uintptr_t sp __asm__( "s1" );

if ( interrupted_stack_at_multitasking_start == 0 ) {
  interrupted_stack_at_multitasking_start = sp;
}
```

Clang honours a local register variable only where it appears as an asm operand, and otherwise
treats reading it as undefined. It deleted the entire conditional:

```
clang  __wrap__RISCV_Interrupt_dispatch:
         j    _RISCV_Interrupt_dispatch        # the whole function

gcc    __wrap__RISCV_Interrupt_dispatch:
         lw   a4, -0x700(gp)
         bnez a4, ...
         sw   s1, -0x700(gp)                   # records the SP
         j    _RISCV_Interrupt_dispatch
```

`interrupted_stack_at_multitasking_start` stayed zero and the assertion at `tc-score-isr.c:274`
failed. Naming the register in the asm template — `__asm__("mv %0, s1" : "=r"(sp))` — works under
both compilers.

**A conservative-looking alternative does not work.** Keeping the register variable and passing it
as a *tied* asm operand (`__asm__("" : "=r"(sp) : "0"(sp_reg))`) compiles, but clang allocates the
pair to `a3` and never moves `s1` into it, so the code reads garbage. Only naming the register in
the template is reliable.

## 8. The scaffolding that manufactured a bug

Recorded in full because it cost real time and because the failure mode is generic.

While the rtems-tools fix (item 4) was still out of reach, the undefined-symbol problem was
bridged with a wrapper around `rtems-syms` that stripped the offending rows out of the generated
table. The `dl*` tests linked. And `dl11`, which had been passing, started failing with
`dlsym ptr_call failed: ret value bad`.

`dl11` checks that a dynamically loaded module and the base image agree on the address of `errno`.
The generated table is a flat list, and the same file contains:

```c
#define RTEMS_TLS_INDEX__tls_errno 348
```

which is a **positional index into that list**. The wrapper deleted two rows that sort before
`_tls_errno`; every TLS symbol after them shifted by two; `_tls_errno` picked up another symbol's
runtime offset; module and base image disagreed about where `errno` lives.

So a workaround manufactured a failure that looked exactly like a compiler bug, in a test
specifically about TLS, during a session in which two genuine TLS bugs had already been found.
Rewriting the wrapper to zero the value and keep the row fixed it immediately; doing the job
properly in rtems-tools made the whole question moot.

The generalisable point is not "avoid scaffolding". It is that **scaffolding belongs on the
suspect list with everything else**, and that a workaround which silently renumbers data is far
more dangerous than one which fails loudly.

## Making the rtems-tools fix survive a rebuild

`rtems-syms` is built and installed by the RTEMS Source Builder. Installing a locally built copy
into `~/rtems/7` fixes today and loses the fix the next time anyone runs the RSB — silently
reintroducing a link failure with no obvious cause.

The RSB pins rtems-tools by commit in `rtems/config/tools/rtems-tools-7.cfg` and supports
`%patch add` with a `%hash`, which is how the RTEMS GCC recipes carry their patches. The fix is
wired in there, so an RSB rebuild applies it.

One trap worth recording: **`rtems/patches/` in the RSB is gitignored** — it is a download cache,
not a source directory (43 files present, none tracked). A patch dropped there and referenced by
`file://` works on the machine it was created on and silently vanishes on a fresh checkout. The
change therefore has to be carried as a tracked patch against the RSB tree, which is the
convention the builder repo already used for its macOS zlib fix.

Verified by reverting, forward-applying the tracked patch, rebuilding `7/rtems-tools` through the
RSB, and confirming the resulting `rtems-syms` emits no undefined symbols and matches a direct
build.

A red herring cleared up along the way: the installed `rtems-syms` reports version
`7.105f43d299cb-modified`, and `105f43d299cb` is **not an rtems-tools commit** — it is the RSB's
own commit hash. `rtemstoolkit/version.py` derives the version from
`git.repo(dirname(sys.argv[0]))`, and the RSB unpacks and builds rtems-tools *inside the RSB git
tree*, so the repo walk finds the RSB and stamps its HEAD. That is also why the GCC and
`rtems-syms` version strings carry the identical hash and the identical `-modified` suffix: the
same dirty worktree reported twice. The RSB actually pins rtems-tools at
`2fdcd1ed953ed40b51f12be71968929cd88dadae`.

## What parity does not mean

The table at the top is a real result and a narrow one.

**There is still no Clang driver for RTEMS.** Everything `-qrtems` does in GCC is hand-rolled in
[`repro/config.ini`](repro/config.ini): the sysroot, `-nostartfiles` plus four explicit crt
objects, three `-L` paths, `-stdlib=libstdc++` with three `-isystem` directories of GCC's C++
headers, `-ftls-model=local-exec`, and a hand-repacked `libgcc_eh.a`. Every path is absolute and
machine-specific. Tier A of [04](04-llvm-gap-analysis.md) is untouched.

**The runtime is still GCC's.** newlib, libgcc, libgcc_eh, libstdc++ and the crt files all come
from the RSB-built GCC. The only LLVM runtime component is `libclang_rt.builtins.a`. This is Clang
as a code generator against GNU's runtime, not an LLVM toolchain. Nothing in Tier B has been
tested, including the one-line `libcxx/__config` change identified as the highest-value item there.

**The harness is not RTEMS's.** 674 is what `tools/run-all.sh` found and classified. The project's
own `rtems-test` may disagree, and the seven shared failures deserve explanation rather than the
assumption that they are timeouts. The 25 XFAILs are unaudited — some could be Clang-specific
damage wearing an expected-failure label.

**`-ftls-model=local-exec` is still forced** and it is not known whether Clang's default works now
that the TLS alignment bug is fixed. It was never re-tested.

**Nothing is upstream.** Every fix in this document lives on a branch.

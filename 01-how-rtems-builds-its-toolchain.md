# How RTEMS builds and patches its toolchain

Everything here is from the RTEMS Source Builder (RSB) at commit `105f43d`, RTEMS 7.

## The short version

RTEMS does not fork GCC or binutils. It **builds upstream releases from a recipe**, applying
a small number of patches by URL, with SHA-512 pinning. Of 13 architectures in RTEMS 7,
**11 use entirely stock upstream tools**. One (nios2) is version-frozen but unpatched. One —
**MicroBlaze — is the only architecture with a genuinely non-upstream toolchain.**

## The build system

Two file types, parsed by different code:

| | `.bset` / `.binc` | `.cfg` |
|---|---|---|
| parser | `source-builder/sb/setbuilder.py` (`buildset.parse()`, lines 300–405) | `source-builder/sb/config.py` (`class file`, line 248+) |
| purpose | an ordered **list of things to build** plus macro settings | a **recipe** for one package |
| grammar | `package:`, `%define`, `%defineifnot`, `%undefine`, `%include`, `%patch`, `%source`, `%hash` — nothing else | full set, including `%if`/`%ifos`, `%prep`, `%build`, `%install` |

### The override mechanism is one rule

`%define` is unconditional. `%defineifnot` is first-wins. That single asymmetry is the whole
configuration system.

`rtems/config/7/rtems-microblaze.bset` in its entirety:

```
%define release 1
%define rtems_arch microblaze

# Microblaze gcc and binutils are not upstream
%define with_rtems_binutils tools/rtems-xilinx-binutils-2.36
%define with_rtems_gcc tools/rtems-xilinx-gcc-12-newlib-head

%include 7/rtems-default
```

`rtems/config/7/rtems-default.binc` then says:

```
%defineifnot with_rtems_binutils tools/rtems-binutils-2.46.1
%defineifnot with_rtems_gcc      tools/rtems-gcc-15.2-newlib-head
```

Both are no-ops because the arch bset already `%define`d them. The other six `%defineifnot`
lines take effect. That is the entire override.

**Side effect worth knowing:** `--with-rtems-gcc=X` on the command line becomes a macro in
the *defaults* map before any file is read (`source-builder/sb/options.py:316–322`), so it
beats `%defineifnot` — but an arch bset using plain `%define` **overwrites the command-line
value**. For microblaze, aarch64 and nios2, `--with-rtems-gcc` silently does nothing.

### Patches are URLs, not vendored files

`rtems/patches/` is **gitignored** (`rtems/.gitignore` line 3). It is a download cache, not a
source of truth. The authoritative list is the `%patch add` lines in the `.cfg` files.

```
%patch add binutils -p1 %{xilinx_github_url}/rel-v2021.1/meta-microblaze/.../0001-....patch
%hash sha512 0001-....patch 56971b06821d7ab36b068016dde6086941121fca40d661056fe...
```

Every downloaded source and patch **must** carry a `%hash sha512` or the build fails
(`download.py:130` — `raise error.general('%s: no hash found')`). Only SHA-512 is accepted;
MD5 and SHA-1 are hard errors. A digest mismatch deletes the file and fails.

At `%prep` time each patch becomes literally `cat <file> | patch -p1`
(`source-builder/sb/build.py:462–464`), applied in definition order.

Three patch hosts are in use:

- `raw.githubusercontent.com/Xilinx/meta-xilinx/<tag>/…` — MicroBlaze only
- `gitlab.rtems.org/…/assets/tracmigration/ticket_attachments/…` — legacy Trac attachments
- `gitlab.rtems.org/-/project/<N>/uploads/<sha>/…` — newer GitLab issue uploads

So RTEMS's toolchain integrity depends on third-party URLs remaining live, with SHA-512 as the
only defence against mutation. It is reproducible as long as the hosts are up.

### Substituting a source tarball

`%source set` is **first-wins and silently ignored if already set**. The idiom is: set your URL
first, then include the generic config whose own `%source set` is then discarded.
`rtems-binutils-2.36.cfg` sets a `gitlab.rtems.org` git-snapshot URL, then includes
`binutils-2-1.cfg` whose `https://ftpmirror.gnu.org/...` line is silently dropped.

## Patch inventory — 42 files

### binutils / MicroBlaze — 13 patches, from `meta-xilinx` `rel-v2021.1`

| # | What it does |
|---|---|
| 1 | adds `wdc.ext.clear` / `wdc.ext.flush` opcodes |
| 2 | accepts `-mlittle-endian`/`-mbig-endian` as no-op assembler options |
| 3 | **suppresses the "no .eh_frame_hdr table" error** when the arch is MicroBlaze — marked FIXME |
| 4 | adds reloc `R_MICROBLAZE_32_NONE` (33) so relaxation cannot corrupt assembler-resolved locals |
| 5 | **reverts an upstream `elf_gc_sweep_symbol` change** that segfaulted shared-libstdc++ C++ apps |
| 6 | fixes little-endian addend placement in the TLS TPREL path |
| 7 | adds address-extension instructions (`lbuea`, `lhuea`, `lwea`, …) |
| 8 | `MAX_OPCODES` 291 → 299 (fixup for #7) |
| 9 | adds `bsefi`/`bsifi` bit-field instructions |
| 10 | stops `imm -1` being wrongly relaxed away |
| 11 | **generic gas**: handles a trailing `U`/`u` suffix in `operand()` |
| 12 | casts to `int` in `parse_imm`, killing spurious range errors |
| 13 | **generic ld**: special-cases `R_MICROBLAZE_SRW32` overflow with `-mxl-gp-opt` advice |

**Four of thirteen (#3, #5, #11, #13) modify architecture-neutral binutils code.** That is why
MicroBlaze cannot use the stock binutils recipe. Ordering is load-bearing: #1 sets
`MAX_OPCODES` to 291, #7 adds instructions, #8 corrects to 299, #9 raises it again.

Patch #3 is directly relevant to work done elsewhere in this repository's sibling project: it
*silences* the `.eh_frame` warning rather than fixing the underlying MicroBlaze GAS behaviour
of emitting `R_MICROBLAZE_NONE` markers for resolved label differences.

### GCC / MicroBlaze — 13 Xilinx patches, from `meta-xilinx` `xlnx-rel-v2024.1`

Seven are testsuite-only. Six are real:

| What it does |
|---|
| rewrites `atomic_compare_and_swapsi` so bool out, val out and memory are visible side effects |
| fixes CAS boolean polarity |
| fixes an ICE with `-msmall-divides -mxl-barrel-shift` |
| adds `*ashrsi3_with_size_opt` for constant shifts > 5 under `-Os` |
| `FUNCTION_PROFILER`: `brki r16,_mcount` → `bralid r15,_mcount` |
| deletes the buggy hand-written `__moddi3` assembly, falling back to libgcc's C version |

Note the two Xilinx tags **do not match**: binutils patches come from the 2021.1 Vivado branch,
GCC patches from 2024.1. They were adopted three years apart and the binutils set has not been
refreshed since.

One testsuite patch (`0001-LOCAL-Testsuite-builtins-tests-require-fpic.patch`) ships with
**literal unresolved git conflict markers** that land verbatim in the `.exp` file; a later patch
in the same series cleans them up. The two must be applied together and in order.

### GCC / MicroBlaze — 3 RTEMS-authored patches

Define `__USE_INIT_FINI__`, add single-underscore `_init`/`_fini` aliases to `crti.S`, and
define `__ELF__`. Both `rtems.h` patches are diffed against the same base blob and insert into
the same three-line context window — applied in order, the second **relies on `patch` fuzz** to
apply.

### GCC — 4 architecture-generic RTEMS patches

| What it does |
|---|
| back-ports `gcov-tool merge-stream` to GCC 12 (~1700 lines) |
| adds `SUBTARGET_CC1_SPEC` threading and defaults RTEMS to `-ftls-model=local-exec` (18 files) |
| enables TLS and a dozen `HAVE_*` features in libstdc++ for RTEMS (155 KB, mostly regenerated `configure`) |
| GCC-15 successor to the above, enabling `std::filesystem` |

### GDB — 2, RISC-V — 1, host build fixes — 6

The GDB one adds a real `gdb/rtems-tdep.c` (90 lines) that sniffs the `.rtemsroset` ELF section
and registers a TLS load-module-address hook. The host fixes are Apple-Silicon `config.sub`
updates for isl/mpc and a macOS zlib workaround.

## Which architectures need patched tools

| Arch | binutils | gcc | Status |
|---|---|---|---|
| aarch64 | 2.46.1 | 15.2 (pinned) | stock |
| arm, i386, m68k, moxie, or1k, powerpc, riscv, sparc, x86_64, mips | 2.46.1 | 15.2 | **stock** |
| nios2 | 2.43 | 14.4 | stock sources, version-frozen (deprecated arch) |
| **microblaze** | **2.36.1** | **12.4.1** | **patched — 26 arch patches + 3 RTEMS patches** |

The bsets say so themselves. `rtems-microblaze.bset:4`: `# Microblaze gcc and binutils are not
upstream`. `rtems-nios2.bset:6–8`: *"Nios2 support has been deprecated… These are the last
versions to include nios2 support."*

### The MicroBlaze version gap

| Component | MicroBlaze | RTEMS 7 default | Gap |
|---|---|---|---|
| GCC | 12.4.1 | 15.2.0 | **3 major releases**, ~3 years |
| binutils | 2.36.1 | 2.46.1 | **10 minor releases**, ~5 years |
| gdb | 17.2 | 17.2 | none |
| newlib | `7d4336cf` | `7d4336cf` | none |

The old toolchain drags its own workarounds along: the `mpc 1.2.1` / `isl 0.24` versions bound
to the GCC 12 path are exactly the ones needing Apple-Silicon `config.sub` patches, and the
2.36-era bundled zlib is what forced the `-Dfdopen=fdopen` hack in `binutils-2-1.cfg:63–67`.
Neither is needed on the GCC 15 / binutils 2.46 path.

## Version pinning and release channels

`rtems/config/7/rtems-default.binc` is the single pinning point for the whole stack: GCC 15.2 +
newlib head, binutils 2.46.1, gdb 17.2, dtc 1.7.2, expat 2.8.1, gmp 6.3.0, mpfr 4.2.1.

Three parallel channels differ only in those pins:

| channel | gcc | binutils | gdb |
|---|---|---|---|
| `7/` | 15.2 | 2.46.1 | 17.2 |
| `next/` | 15.3 | 2.46.1 | 17.2 |
| `edge/` | head | head | head |

`head` is **not** a live clone — it pins a git snapshot SHA against an RTEMS-hosted mirror with
a `%hash`, so it is still bit-for-bit reproducible. newlib is *always* in snapshot form; there
is no newlib release pin anywhere.

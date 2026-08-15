# Buildroot MicroBlaze: the soft-instruction tax, measured and fixed

**Status: patch DRAFT — NOT SENT anywhere, per instruction.** This documents
the finding, the measured fix rounds, and the patch outline for review.

## The finding (from 10-mb-boot-profile.md)

A tcgcov-counted boot of `qemu_microblazeel_mmu_defconfig` showed the
guest spending large fractions of its boot work in software fallbacks for
instructions the emulated CPU has:

* QEMU's MicroBlaze CPU model defaults: `use-div=true`, `use-hw-mul=2`,
  `use-barrel=true`, `use-pcmp-instr=true`, `use-fpu=2` (the petalogix
  board overrides only version/endianness).
* The **upstream kernel's own `mmu_defconfig`** agrees: `USE_DIV=1`,
  `USE_HW_MUL=2`, `USE_FPU=2`.
* Buildroot's `board/qemu/microblazeel-mmu/linux.config` **omits**
  `USE_DIV`/`USE_FPU` (Kconfig defaults them to 0) — so the Buildroot QEMU
  kernel is built soft-div against a CPU with a divider.
* Userland is worse: GCC's MicroBlaze defaults are soft-mul, soft-div,
  no barrel shifter, no pattern compare — and Buildroot passes no
  MicroBlaze feature flags. Verified by disassembly: the shipped
  `ld.so.1`/`libc.so.6`/busybox contain **zero** `mul`, `bsll*`, `pcmpbf`
  or `idiv` instructions.

The measured cost at boot (206.8M TB execs to login): `ld.so __umodsi3`
7.5M (ELF symbol-hash modulo), `__mulsi3` 1.6M, kernel soft-div loops
~1.0M — plus 21.6M in initramfs gunzip and 4.2M in `calibrate_delay`.

## Fix rounds (each verified by disassembly before booting)

| round | change | result |
|---|---|---|
| 1 | kernel `USE_DIV=1`; initramfs gzip→LZ4; `BR2_TARGET_OPTIMIZATION="-mno-xl-soft-div"` | kernel got 583 `idiv`; **userland got nothing** — `BR2_TARGET_OPTIMIZATION` is baked into the *toolchain wrapper binary* at wrapper build time (`toolchain-wrapper.mk:22`), so setting it after the toolchain exists is a silent no-op |
| 2 | wrapper rebuilt with the flag baked in (defines replicated from `toolchain-wrapper.mk`/`gcc.mk`; flag proven active on a test fn before building); glibc+busybox from clean | ld.so 40 / libc 291 / busybox 192 `idiv` instructions |
| 3 | full flag set matching the CPU model: `-mno-xl-soft-mul -mno-xl-soft-div -mxl-barrel-shift -mxl-pattern-compare` | **in progress** — numbers to be added |

**Round-2 re-measurement** (same boot-to-login methodology, plus
`lpj=836608` preset):

| bucket | before | after r2 | delta |
|---|---:|---:|---:|
| non-idle boot work | 92.6M TB execs | 62.6M | **−32.4%** |
| initramfs decompress | 21.6M (gunzip) | 4.4M (LZ4) | −79% |
| `calibrate_delay` | 4.2M | ~0 (`lpj=` preset) | −100% |
| kernel soft-div loops | 1.02M | 0.15M (residual = 64-bit `__div64_32`) | −86% |
| `ld.so __umodsi3` | 7.5M | gone from profile | −100% |

Still visible after round 2 and addressed by round 3: `ld.so __mulsi3`
1.6M (soft multiply — present before too, hidden below the top-5 cutoff),
byte-wise `strcmp` 5.7M (pattern-compare candidate), inline shift loops
(invisible to symbol profiles — no helper call — but real).

## The patch outline (draft, not sent)

Four files in Buildroot, EL tested / BE by symmetry:

1. `board/qemu/microblazeel-mmu/linux.config`:
   add `CONFIG_XILINX_MICROBLAZE0_USE_DIV=1` — aligns with the kernel's
   own `mmu_defconfig` and QEMU's CPU.
2. `board/qemu/microblazebe-mmu/linux.config`: same.
3. `configs/qemu_microblazeel_mmu_defconfig`:
   `BR2_TARGET_OPTIMIZATION="-mno-xl-soft-mul -mno-xl-soft-div
   -mxl-barrel-shift -mxl-pattern-compare"`.
4. `configs/qemu_microblazebe_mmu_defconfig`: same.

Notes for the eventual submission:

* The defconfig route works for fresh builds because the wrapper is built
  *after* the defconfig is applied; the round-1 trap only bites when
  changing the value on an existing tree. Worth one sentence in the
  commit message so users rebuilding in place are not surprised.
* Deliberately NOT in the patch: `USE_FPU` (kernel does no FP; userland
  FP ABI is a toolchain-wide decision), LZ4 initramfs and `lpj=` (real
  wins, but image-content/runtime choices, not instruction-set
  correctness), and `-mxl-multiply-high` (QEMU has `use-hw-mul=2` but the
  board kernel config says `HW_MUL=1`; keep the flag set conservative).
* These defconfigs exist to run on QEMU's petalogix model, which is what
  makes the instruction set knowable — the same argument does not extend
  to generic MicroBlaze targets, where FPGA configs vary.

## Measurement method

tcgcov (`ctx=on,mode=tb`, counts) under the PoC QEMU from the
context-visibility work; boot stopped at the login prompt; symbols via
System.map + `nm` of the exact built binaries; every flag change verified
by instruction-count disassembly *before* booting, so a silent no-op
build (round 1's lesson) cannot masquerade as a null measurement result.

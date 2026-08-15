# MicroBlaze Linux boot profile: what is hot, and which hardware is unused

Measured 2026-08-14 with tcgcov (`ctx=on,mode=tb`, counts) on the PoC QEMU
(10.2.4 + context patches), `petalogix-s3adsp1800`, Buildroot glibc rootfs,
Linux 6.12.81. One boot, stopped at the login prompt (8 s wall): **206.8M
translation-block executions** — kernel 172.8M (83.6%), user 34.0M.
Symbols attributed via System.map, `busybox_unstripped` and the staging
`ld.so.1`/`libc.so.6` (libc window base-fit at 0x48003000).

## Where the boot goes

| bucket | TB execs | % of non-idle (92.6M) |
|---|---:|---:|
| idle spin (`do_idle` + `default_idle_call` + `arch_cpu_idle*`) | 114.2M | — (55% of total) |
| **dynamic linking** (`ld.so`: `do_lookup_x` 7.8M, `__umodsi3` 7.5M, `strcmp` 5.7M, `_dl_relocate…` 1.7M, …) | ~27M | **29%** |
| **initramfs gunzip** (`inflate_fast` 20.0M + zlib tables) | 21.6M | 23% |
| memset + memcpy | 9.8M | 11% |
| **software division, everywhere** (see below) | 8.8M | **9.5%** |
| `calibrate_delay` | 4.2M | 4.6% |
| soft float (`__fpcmp_parts_d` + friends) | ~1M | 1% |

## The hardware-acceleration finding

The QEMU CPU model has everything on by default
(`target/microblaze/cpu.c`: `use-div=true`, `use-fpu=2`, `use-hw-mul=2`,
`use-barrel=true`, `use-pcmp-instr=true`; the petalogix board overrides
only `version=7.10.d` and endianness). The software stack does not use two
of them:

| feature | HW present (QEMU) | software built with | evidence in profile |
|---|---|---|---|
| **divider** | yes | `CONFIG_XILINX_MICROBLAZE0_USE_DIV=0`, userland `-mxl-soft-div` | `ld.so __umodsi3` **7.5M** (ELF symbol hash = modulo per lookup); kernel soft-div loop labels `mod_too_small`/`div2`/`div1`/`div0` ~0.9M; busybox `__umodsi3`+`__udivsi3` 163K. Total ~**8.8M TB execs ≈ 9.5% of boot work** |
| **FPU** | yes (use-fpu=2) | `CONFIG_XILINX_MICROBLAZE0_USE_FPU=0` | soft-float `__fpcmp_parts_d` 705K in libc (locale/strtod paths); ~1% at boot, matters more for FP workloads |
| barrel shifter | yes | enabled (=1) | no `__ashlsi3`/soft-shift symbols — confirmed in use |
| hw multiplier | yes | enabled (=1) | no `__mulsi3`; only 64-bit `__muldi3` helper (166K), normal |
| pcmp | yes | enabled (=1) | — |
| clz (v8.10+) | n/a on 7.10.d | — | `__ctzsi2` 471K in ld.so, minor |

The `USE_DIV=0`/`USE_FPU=0` values come from the Buildroot
`qemu_microblazeel_mmu_defconfig` kernel config being conservative about
the original Spartan-3A DSP 1800 reference design. Under QEMU the divider
and FPU are free.

## Recommendations, by expected boot-work saved

1. **Enable the hardware divider** (kernel: `CONFIG_XILINX_MICROBLAZE0_USE_DIV=1`;
   userland: drop `-mxl-soft-div` so glibc/ld.so/busybox emit `idiv`):
   ~9% of boot compute, and the ld.so half of it multiplies with every
   `exec()` after boot, since every symbol lookup hashes with a modulo.
2. **Kill the dynamic-linking tax** where acceptable: static busybox (or
   musl/uclibc) removes ~29% of non-idle boot work — not a hardware issue,
   but the single biggest lever, and it subsumes most of the ld.so soft-div.
3. **Initramfs compression**: gunzip is 23% of boot work with barrel+mul
   already on; LZ4 or an uncompressed initramfs trades image size for most
   of it.
4. **`lpj=` preset** on the cmdline removes `calibrate_delay` (4.6%).
5. FPU: enable when the real FPGA has it; at boot it is only ~1%.

Exception paths are *not* hot at boot: the whole
`_hw_exception_handler`/unaligned window shows ~2K executions, and
`_tlbia_1` (TLB flush loop, 64 iterations each) 121K — the software TLB
regime is fine at this scale.

## Method note

The tcgcov TCGCOV2 artifact carries per-(context, TB) execution counts for
the entire boot from instruction zero — including `_start` at the reset
vector and the pre-MMU window — with no guest instrumentation. The same
run distinguishes ld.so from libc from busybox per context; the numbers
above aggregate across contexts.

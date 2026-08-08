# Reproducing the Clang RISC-V bring-up

## 1. Build Clang with the RISC-V backend

Stock upstream `llvm-project` works; you only need `RISCV` in the target list.

```sh
cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64;ARM;RISCV"
ninja -C build clang lld llvm-ar llvm-ranlib llvm-objcopy llvm-nm llvm-objdump llvm-readelf llvm-size llvm-strip
```

Then apply [`../patches/llvm/0001-Clang-RTEMSTargetInfo-for-RISC-V.patch`](../patches/llvm/) and
rebuild `clang`. Without it, `__rtems__` is never defined for RISC-V and nothing below works.

Verify:

```sh
build/bin/clang --target=riscv32-unknown-rtems7 -dM -E - </dev/null | grep rtems
# #define __rtems__ 1
```

## 2. Patch RTEMS

Apply both patches in [`../patches/rtems/`](../patches/rtems/) to an RTEMS 7 tree. Do this in a
`git worktree` if you have an existing GCC build you care about — `waf configure` will otherwise
overwrite `build/<arch>/<bsp>` in place.

## 3. The `ld` shim

Clang's `BareMetal` ToolChain resolves a bare `ld` off `PATH`, and RTEMS's incremental-link (`-r`)
tasks pass only `ABI_FLAGS`, so `-fuse-ld=lld` in `LDFLAGS` does **not** reach them. On any host
whose `/usr/bin/ld` is not a RISC-V-capable GNU ld — macOS in particular — shim it:

```sh
mkdir -p ldshim && ln -sf /path/to/llvm/build/bin/ld.lld ldshim/ld
export PATH="$PWD/ldshim:/path/to/llvm/build/bin:$HOME/rtems/7/bin:$PATH"
```

## 4. Configure and build

Copy [`config.ini`](config.ini) into the RTEMS tree and expand `${HOME}` to real paths — it is
checked in with placeholders. It assumes an RSB-installed `riscv-rtems7` GCC 15.2.0 toolchain at
`$HOME/rtems/7`, because the build borrows that toolchain's newlib, libgcc and libstdc++.

```sh
./waf configure
./waf -j8
```

Expect **712 of 721** executables. The `dl*` `.rap` modules and one TLS link still fail.

## 5. Check the image before trusting it

This step is not optional. A successful link does **not** mean a working image — see failure 12 in
[05](../05-clang-riscv-bringup.md).

```sh
llvm-readelf -h build/riscv/mbv/testsuites/samples/hello.exe | grep Entry
# Entry point address: 0x80000000     <- correct
# Entry point address: 0x0            <- linkcmds was not applied; stop here
```

## 6. Run

```sh
qemu-system-riscv32 -M amd-microblaze-v-generic -m 256m \
  -display none -monitor none -serial file:hello.txt -no-reboot \
  -icount shift=0,sleep=off \
  -device loader,file=build/riscv/mbv/testsuites/samples/hello.exe,cpu-num=0
```

## What the `config.ini` workarounds correspond to

Each line exists because nothing in the Clang path computes it. They are **not** proposed fixes —
they are a hand-written sketch of what an `RTEMS.cpp` ToolChain would need to do.

| setting | stands in for |
|---|---|
| `WARNING_FLAGS` without `-Werror` | clang-only diagnostics (`-Wsign-compare`, `-Wdefault-const-init-var-unsafe`) |
| `CC_WARNING_FLAGS` without `-Wold-style-declaration` | a clang-conditional warning spec, which does not exist |
| `--sysroot=` | the sysroot a target-configured GCC has baked in |
| `-idirafter .../include` | `gcov.h`, a GCC-provided header |
| `-rtlib=libgcc` + `-L` | missing `libclang_rt.builtins.a` for RTEMS |
| `-nostartfiles` | `%{!qrtems:crt0%O%s}` — suppressing newlib's stub `crt0.o` |
| `-L .../rv32imafc/ilp32f` | multilib selection, which Clang does not infer here |
| `-stdlib=libstdc++` + 3 × `-isystem` | no libc++ for RTEMS |
| `-fuse-ld=lld` | the host `ld` cannot link RISC-V ELF |
| `-T linkcmds` | `%{qrtems:...%{!qnolinkcmds:-T linkcmds%s}}` |

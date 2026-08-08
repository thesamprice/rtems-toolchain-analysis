#!/bin/bash
# Build compiler-rt's builtins for riscv32-unknown-rtems7 and install them where
# clang looks for them.
#
# Why this matters (see BUGS.md O1): GCC's libgcc declares its soft-float
# helpers GLOBAL HIDDEN, and lld demotes hidden symbols to STB_LOCAL in a static
# link. rtems-syms harvests only *global* symbols from the libdl base image, so
# under lld those helpers vanish from the runtime linker's table and modules
# fail with "unresolved externals".
#
# COMPILER_RT_BUILTINS_HIDE_SYMBOLS defaults to ON. Turning it OFF yields
# GLOBAL DEFAULT symbols, which rtems-syms can then export.
#
# Two details that are easy to get wrong:
#   * CMAKE_SYSTEM_NAME=Generic is required, otherwise CMake treats this as a
#     host build on macOS and takes the Darwin universal-binary path.
#   * compiler-rt has no unwinder, so C++ links still need libgcc's
#     _Unwind_*. This toolchain has no libgcc_eh.a, so extract one — see below.

set -eu

: "${LLVM_BUILD:?set LLVM_BUILD to your llvm build dir}"
: "${LLVM_SRC:?set LLVM_SRC to your llvm-project checkout}"
: "${RTEMS_PREFIX:=$HOME/rtems/7}"

TARGET=riscv32-unknown-rtems7
ARCH_FLAGS="-march=rv32imafc_zicsr_zifencei -mabi=ilp32f"
MULTILIB=rv32imafc/ilp32f
BUILD=${BUILD:-$PWD/crt-build}

cmake -S "$LLVM_SRC/compiler-rt/lib/builtins" -B "$BUILD" -G Ninja \
  -DCMAKE_SYSTEM_NAME=Generic \
  -DCMAKE_SYSTEM_PROCESSOR=riscv32 \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_COMPILER="$LLVM_BUILD/bin/clang" \
  -DCMAKE_ASM_COMPILER="$LLVM_BUILD/bin/clang" \
  -DCMAKE_AR="$LLVM_BUILD/bin/llvm-ar" \
  -DCMAKE_RANLIB="$LLVM_BUILD/bin/llvm-ranlib" \
  -DCMAKE_NM="$LLVM_BUILD/bin/llvm-nm" \
  -DCMAKE_C_COMPILER_TARGET="$TARGET" \
  -DCMAKE_ASM_COMPILER_TARGET="$TARGET" \
  -DCMAKE_SYSROOT="$RTEMS_PREFIX/riscv-rtems7" \
  -DCMAKE_C_FLAGS="$ARCH_FLAGS" \
  -DCMAKE_ASM_FLAGS="$ARCH_FLAGS" \
  -DCOMPILER_RT_BAREMETAL_BUILD=ON \
  -DCOMPILER_RT_BUILTINS_HIDE_SYMBOLS=OFF \
  -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
  -DCOMPILER_RT_OS_DIR=rtems \
  -DLLVM_CMAKE_DIR="$LLVM_SRC/llvm/cmake/modules"

ninja -C "$BUILD"

# Install where clang's driver looks for it.
DEST="$LLVM_BUILD/lib/clang/23/lib/$TARGET"
mkdir -p "$DEST"
cp "$BUILD/lib/rtems/libclang_rt.builtins-riscv32.a" "$DEST/libclang_rt.builtins.a"

# Verify the whole point of the exercise:
"$LLVM_BUILD/bin/llvm-readelf" -s "$DEST/libclang_rt.builtins.a" \
  | grep -E "FUNC.*__muldf3" | head -1
echo "^ must say GLOBAL DEFAULT, not GLOBAL HIDDEN"

# compiler-rt ships no unwinder and this toolchain has no libgcc_eh.a, so build
# a minimal one from libgcc's unwind objects. Linking all of libgcc instead
# would reintroduce its hidden __muldf3 and undo the fix.
GCCLIB="$RTEMS_PREFIX/lib/gcc/riscv-rtems7/15.2.0/$MULTILIB"
EH=${EH:-$PWD/ehlib}
rm -rf "$EH"; mkdir -p "$EH/x"
( cd "$EH/x" && "$LLVM_BUILD/bin/llvm-ar" x "$GCCLIB/libgcc.a" \
    unwind-dw2.o unwind-dw2-fde.o unwind-c.o )
"$LLVM_BUILD/bin/llvm-ar" rcs "$EH/libgcc_eh.a" \
  "$EH/x/unwind-dw2.o" "$EH/x/unwind-dw2-fde.o" "$EH/x/unwind-c.o"

echo "Add to the RTEMS build: -rtlib=compiler-rt, -L$EH and -lgcc_eh"

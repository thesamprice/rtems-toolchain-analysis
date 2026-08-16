#!/bin/sh
# Demonstrate the arch/microblaze/kernel/entry.S defect of PR 121432, and the
# fix, with no Linux involved.  Needs a microblaze GCC and qemu-system-microblaze.
#
#   ./run.sh                      uses microblaze-rtems7-gcc
#   GCC=microblaze-elf-gcc ./run.sh
set -eu
: "${GCC:=microblaze-rtems7-gcc}"
: "${QEMU:=qemu-system-microblaze}"
cd "$(dirname "$0")"

for fix in 0 1; do
    $GCC -O2 -DFIX_ARGS_AREA=$fix -nostdlib -nostartfiles -T mb.ld \
        start.S entry_repro.S entry_repro_main.c sink.c -o er-$fix.elf
    printf 'FIX_ARGS_AREA=%s : ' $fix
    timeout 20 $QEMU -M petalogix-s3adsp1800 -m 256M -nographic -no-reboot \
        -kernel er-$fix.elf 2>/dev/null | head -1
done

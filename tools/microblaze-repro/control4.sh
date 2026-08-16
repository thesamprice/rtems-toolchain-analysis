#!/bin/bash
# CONTROL v4.  The workaround-patched compiler is already built and installed
# (control3 got that far before my mtime guard checked the wrong path), so this
# only rebuilds the kernel with a pristine entry.S and boots it.
#
# This is the question that decides whether the reproduction here is PR 121432.
set -euo pipefail
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    gcc g++ make file wget cpio unzip rsync bc git python3 perl patch \
    libncurses-dev pkg-config which gawk bzip2 xz-utils tar gzip sed \
    diffutils findutils binutils build-essential ca-certificates \
    libelf-dev libssl-dev flex bison sudo qemu-system-misc >/dev/null
id -u br >/dev/null 2>&1 || useradd -m -s /bin/bash br
chown -R br:br /br /out 2>/dev/null || true

sudo -u br bash -eo pipefail <<'EOS'
cd /br/src
G=$(ls -d output/build/host-gcc-final-* | head -1)
echo "workaround refs in gcc source: $(grep -c TARGET_CALLEE_SAVE_COST $G/gcc/config/microblaze/microblaze.cc)"
echo "installed cc1: $(ls -l output/host/libexec/gcc/microblazeel-*/*/cc1 2>/dev/null | awk '{print $6,$7,$8}')"

KDIR=$(ls -d output/build/linux-* | grep -v headers | head -1)
cp /patches/entry.S.pristine "$KDIR/arch/microblaze/kernel/entry.S"
cp /patches/fault.c.debug    "$KDIR/arch/microblaze/mm/fault.c"
sed -i '/^CFLAGS_.*-O0 -fno-inline/d' "$KDIR/arch/microblaze/kernel/Makefile" "$KDIR/arch/microblaze/mm/Makefile" || true
echo "entry.S C_ARG_SIZE (want 0): $(grep -c C_ARG_SIZE $KDIR/arch/microblaze/kernel/entry.S || true)"
echo "fault.c BADFAULT (want 1):   $(grep -c BADFAULT $KDIR/arch/microblaze/mm/fault.c || true)"

set +e
make linux-rebuild all > /out/c4-build.log 2>&1; rc=$?
set -e
echo "BUILD_RC=$rc"
[ "$rc" -eq 0 ] || { tail -25 /out/c4-build.log; exit "$rc"; }

timeout 180 qemu-system-microblazeel -M petalogix-s3adsp1800 -m 256M \
    -kernel output/images/linux.bin -nographic -no-reboot \
    -append "console=ttyUL0,115200 panic=1 print-fatal-signals=1" \
    > /out/boot-control4.log 2>&1 || true
echo "=== control4: workaround compiler, pristine entry.S ==="
grep -A20 BADFAULT /out/boot-control4.log 2>/dev/null | head -26 || tail -14 /out/boot-control4.log
EOS

#!/bin/bash
# Incremental: apply patches to the already-built kernel tree in the brtree
# volume and rebuild just the kernel, reusing the existing cross toolchain.
# Minutes rather than the hour a full Buildroot run takes.
set -euo pipefail

apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    gcc g++ make file wget cpio unzip rsync bc git python3 perl patch \
    libncurses-dev pkg-config which gawk bzip2 xz-utils tar gzip sed \
    diffutils findutils binutils build-essential ca-certificates \
    libelf-dev libssl-dev flex bison sudo qemu-system-misc >/dev/null

id -u br >/dev/null 2>&1 || useradd -m -s /bin/bash br
chown -R br:br /br /out 2>/dev/null || true

sudo -u br O0="${O0:-}" TRACE="${TRACE:-}" bash -eo pipefail <<'EOS'
cd /br/src
KDIR=$(ls -d output/build/linux-* 2>/dev/null | grep -v headers | head -1)
[ -n "$KDIR" ] || { echo "no kernel tree built yet"; exit 1; }
echo "kernel tree: $KDIR"

# Install the fully-patched file outright.  patch --forward silently skips a
# file that already carries an earlier revision of the same change, which
# hides new hunks; copying is unambiguous.
if [ -f /patches/entry.S.full ]; then
    echo "=== installing patched entry.S ==="
    cp /patches/entry.S.full "$KDIR/arch/microblaze/kernel/entry.S"
fi

if [ -n "${O0:-}" ] && [ -f /patches/o0.sh ]; then
    echo "=== forcing -O0 on entry.S callees ==="
    bash /patches/o0.sh "$KDIR"
fi

if [ -f /patches/fault.c.debug ]; then
    echo "=== installing instrumented fault.c (show_regs on user fault) ==="
    cp /patches/fault.c.debug "$KDIR/arch/microblaze/mm/fault.c"
    grep -c BADFAULT "$KDIR/arch/microblaze/mm/fault.c"
fi

echo "=== confirming the change is in the source ==="
grep -c C_ARG_SIZE "$KDIR/arch/microblaze/kernel/entry.S" || true

set +e
make linux-rebuild all > /out/rebuild.log 2>&1
rc=$?
set -e
echo "REBUILD_RC=$rc"
if [ "$rc" -ne 0 ]; then tail -40 /out/rebuild.log; exit "$rc"; fi

cp -a output/images/. /out/

echo "=== booting patched kernel ==="
QEMU_DBG=""
if [ -n "${TRACE:-}" ]; then
    # int      -- every exception vector taken
    # guest_errors, unimp -- bad guest accesses and unimplemented behaviour
    # Add exec,in_asm for instruction-level detail (very large).
    QEMU_DBG="-d int,guest_errors,unimp -D /out/qemu-trace.log"
fi
# panic=1 makes the kernel reboot on panic, and -no-reboot makes QEMU exit
# when the guest reboots.  So QEMU terminates at the moment of the crash
# instead of spinning in the panic path's __udelay for the whole timeout,
# which is what previously buried the interesting part of the trace under
# tens of thousands of post-mortem timer interrupts.
timeout 180 qemu-system-microblazeel -M petalogix-s3adsp1800 -m 256M \
    -kernel output/images/linux.bin -nographic -no-reboot \
    -append "console=ttyUL0,115200 panic=1 print-fatal-signals=1" $QEMU_DBG \
    > /out/boot-patched.log 2>&1 || true
if [ -n "${TRACE:-}" ]; then
    echo "=== qemu trace: last exceptions before the end ==="
    tail -60 /out/qemu-trace.log 2>/dev/null || true
fi
tail -25 /out/boot-patched.log
EOS

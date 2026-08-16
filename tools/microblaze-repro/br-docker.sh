#!/bin/bash
# Build a MicroBlaze Linux image with GCC 15 in a Linux container, for booting
# with the host's qemu-system-microblaze.
#
# Little-endian, matching PR 121432.  The host has no qemu-system-microblazeel,
# so the boot test runs in here too, using Debian's qemu-system-misc.
#
# /br is a named docker volume so the tree survives the container.  Re-running
# with the same volume skips straight to an incremental build, which is what
# makes iterating on an entry.S patch practical: see rebuild.sh.
#
# Buildroot refuses to run on macOS and refuses to configure as root, so deps
# are installed as root and the build runs as an unprivileged user in the
# container's own filesystem.
set -euo pipefail

apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    gcc g++ make file wget cpio unzip rsync bc git python3 perl patch \
    libncurses-dev pkg-config which gawk bzip2 xz-utils tar gzip sed \
    diffutils findutils binutils build-essential ca-certificates \
    libelf-dev libssl-dev flex bison sudo qemu-system-misc >/dev/null

id -u br >/dev/null 2>&1 || useradd -m -s /bin/bash br
mkdir -p /br /out
chown -R br:br /br /out

sudo -u br bash -eo pipefail <<'EOS'
if [ ! -d /br/src ]; then
  git clone --depth=1 https://gitlab.com/buildroot.org/buildroot.git /br/src
fi
cd /br/src

if [ ! -f .config ]; then
  make qemu_microblazeel_mmu_defconfig
fi

# Drop the workaround patch whose whole purpose is to hide the failure.
rm -f package/gcc/1[56].*/*fix-ira-for-GCC15.patch

# Pin GCC 15 so the failure is reproducible.
sed -i '/^BR2_GCC_VERSION_[0-9]*_X=y/d' .config
echo 'BR2_GCC_VERSION_15_X=y' >> .config
make olddefconfig

echo "=== gcc selected ==="; grep -E '^BR2_GCC_VERSION' .config || true
echo "=== remaining gcc patches ==="; ls package/gcc/1[56].*/ 2>/dev/null || true

set +e
make -j"$(nproc)" > /out/build.log 2>&1
rc=$?
set -e
echo "MAKE_RC=$rc" | tee -a /out/build.log
if [ "$rc" -ne 0 ]; then tail -60 /out/build.log; exit "$rc"; fi

cp -a output/images/. /out/

# Boot it.  The tests end at a shell prompt; capture the console and let the
# watchdog kill it.  A working GCC 14 build reaches user space; PR 121432 says
# GCC 15 hangs right after "Run /init as init process".
echo "=== booting ==="
timeout 180 qemu-system-microblazeel -M petalogix-s3adsp1800 -m 256M \
    -kernel output/images/linux.bin -nographic -no-reboot \
    > /out/boot.log 2>&1 || true
tail -40 /out/boot.log
EOS

ls -la /out/

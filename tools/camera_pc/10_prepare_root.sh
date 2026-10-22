#!/bin/bash
# Camera PC preparation, step 1. Run as root:
#   sudo tools/camera_pc/10_prepare_root.sh
#
# 1. Build/runtime packages (CRB + EPEL), kernel-devel for the running kernel.
# 2. Driver options: DMA_MODE=1 (one DMA per image, required for iXon) with
#    the image buffers allocated by the patched driver
#    (andordrvlx-dma-coherent.patch), so no memmap=/mem= boot parameter.
# 3. Removes a memmap= left by an earlier version of this script: GRUB passed
#    it to the kernel as "memmap=8M" (= use only 8 MB of RAM) and boot hung.
# 4. Makes /etc/rc.d/rc.local executable so the Andor load script runs at boot.
# Then run 20_install_andor_root.sh (no reboot needed).
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  echo "run with sudo" >&2
  exit 1
fi

echo "== packages"
dnf -y install dnf-plugins-core epel-release
dnf config-manager --set-enabled crb
dnf -y install gcc gcc-c++ make cmake ninja-build git patch pciutils \
  "kernel-devel-$(uname -r)" elfutils-libelf-devel \
  zeromq-devel cppzmq-devel spdlog-devel json-devel \
  qt6-qtbase-devel libtiff-devel libpng-devel cfitsio-devel \
  python3-pyzmq python3-numpy python3-pytest

echo "== remove memmap= from the boot configuration"
for f in /boot/loader/entries/*.conf /etc/kernel/cmdline /etc/default/grub; do
  [ -f "$f" ] || continue
  if grep -q 'memmap=' "$f"; then
    sed -i -E 's/ ?memmap=[^ "]*//g' "$f"
    echo "cleaned $f"
  fi
done
if grep -rq 'memmap=' /boot/loader/entries /etc/kernel/cmdline /etc/default/grub \
     2>/dev/null; then
  echo "memmap= is still present:" >&2
  grep -r 'memmap=' /boot/loader/entries /etc/kernel/cmdline /etc/default/grub >&2
  exit 1
fi
grubby --info=ALL | grep -E '^(kernel|args)='

echo "== driver options"
cat >/etc/modprobe.d/andordrvlx.conf <<'EOF'
# Andor PCI driver: one-image DMA (required for iXon). With DMA_ADDRESS=0 the
# patched driver allocates the two image buffers (DMA_SIZE/2 MB each, below
# 4 GB) with dma_alloc_coherent; no memmap= is needed.
options andordrvlx DMA_MODE=1 DMA_SIZE=4
EOF
cat /etc/modprobe.d/andordrvlx.conf

echo "== rc.local"
chmod +x /etc/rc.d/rc.local

echo
echo "IOMMU groups: $(ls /sys/kernel/iommu_groups | wc -l) (0 = IOMMU off)"
echo "Done. Next: sudo tools/camera_pc/20_install_andor_root.sh"

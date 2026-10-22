#!/bin/bash
# Camera PC preparation, step 2. Run as root after 10_prepare_root.sh:
#   sudo AEC_DRIVER_PATCH=<patch> tools/camera_pc/20_install_andor_root.sh [SDK_TARBALL]
#
# Extracts SDK 2.103, applies the driver patch given by AEC_DRIVER_PATCH (image
# DMA buffers from dma_alloc_coherent, 32-bit DMA mask, bus mastering; it is a
# diff of Andor's driver source and so is not in this repository, see
# doc/index.html section 2; AEC_DRIVER_PATCH=none skips it), then runs
# Andor's install_andor for "PCI iXon": builds and loads andordrvlx with
# DMA_MODE=1, installs libandor to /usr/local/lib and the firmware to
# /usr/local/etc/andor. Nothing here talks to the camera head, so the cooler
# is not touched.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  echo "run with sudo" >&2
  exit 1
fi

patch_file=${AEC_DRIVER_PATCH:-}
if [ -z "$patch_file" ]; then
  echo "set AEC_DRIVER_PATCH to the driver patch (or none)" >&2
  exit 1
fi
[ "$patch_file" = none ] || patch_file=$(realpath "$patch_file")
user_home=$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)
tarball=${1:-$user_home/work/Andor-SDK/andor-2.103.30058.0.tar.gz}

if grep -q 'memmap=' /proc/cmdline; then
  echo "memmap= is on the running kernel command line; run 10_prepare_root.sh and reboot" >&2
  exit 1
fi
if ! grep -q 'DMA_MODE=1' /etc/modprobe.d/andordrvlx.conf 2>/dev/null; then
  echo "/etc/modprobe.d/andordrvlx.conf missing; run 10_prepare_root.sh first" >&2
  exit 1
fi

echo "== extract and patch the SDK"
work=$(mktemp -d /tmp/andor-sdk.XXXXXX)
tar -xzf "$tarball" -C "$work"
cd "$work/andor"
[ "$patch_file" = none ] || patch -p1 <"$patch_file"

echo "== install_andor"
# A module from an earlier attempt would keep running; unload it first.
rmmod andordrvlx 2>/dev/null || true
# Answers: 4 = PCI iXon, n = do not read INSTALL, n = do not read notes.
if ! printf '4\nn\nn\n' | ./install_andor; then
  echo "install_andor failed; driver build log:" >&2
  cat src/driver/build.txt >&2 || true
  exit 1
fi
echo "driver build log: $work/andor/src/driver/build.txt"

echo "== driver"
lsmod | grep andordrvlx
for p in DMA_MODE DMA_ADDRESS DMA_SIZE; do
  echo "$p=$(cat /sys/module/andordrvlx/parameters/$p 2>/dev/null)"
done
ls -l /dev/andordrvlx*
dmesg | grep andordrvlx | tail -5
for dev in $(lspci -D -d 149a: | cut -d' ' -f1); do
  lspci -s "$dev" -vv | grep -E 'Control:|Region|Interrupt'
done

echo
echo "== boot-time loading (install_andor's rc.local entry is broken)"
"$here/25_driver_service_root.sh"

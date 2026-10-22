#!/bin/bash
# Camera PC: load the Andor PCI driver at boot with a systemd unit.
#   sudo tools/camera_pc/25_driver_service_root.sh
#
# install_andor appends "sh /etc/andordrvlx_load DMA_MODE=1" to
# /etc/rc.d/rc.local, but its prompts also wrote `more` output and our piped
# answers in front of "#!/bin/bash", so rc-local.service fails with
# "Exec format error" and the driver is not loaded after a reboot.
# This script repairs rc.local (backup: rc.local.bak-andor), removes the
# Andor lines from it, and installs andordrvlx.service instead.
# 20_install_andor_root.sh runs it at the end.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  echo "run with sudo" >&2
  exit 1
fi

rc=/etc/rc.d/rc.local
if [ -f "$rc" ]; then
  cp -a "$rc" "$rc.bak-andor"
  # Keep from the shebang on; drop the Andor comment and load line.
  awk 'found || /^#!/ {found = 1; print}' "$rc.bak-andor" |
    grep -v -e '^#Run Andor Technology Startup Script' \
            -e '^sh /etc/andordrvlx_load' >"$rc"
  chmod +x "$rc"
  echo "== $rc repaired (backup $rc.bak-andor)"
  head -1 "$rc"
fi

cat >/etc/systemd/system/andordrvlx.service <<'UNIT'
[Unit]
Description=Andor PCI camera driver (andordrvlx, DMA_MODE=1)
After=systemd-modules-load.service

[Service]
Type=oneshot
RemainAfterExit=yes
# Loads the module (options in /etc/modprobe.d/andordrvlx.conf) and creates
# /dev/andordrvlx0 and /dev/andordrvlx1.
ExecStart=/bin/sh /etc/andordrvlx_load DMA_MODE=1

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
systemctl enable andordrvlx.service
# Already loaded (e.g. right after install_andor): only mark the unit active.
if lsmod | grep -q '^andordrvlx'; then
  rmmod andordrvlx
fi
systemctl start andordrvlx.service
systemctl --no-pager status andordrvlx.service | head -8
systemctl reset-failed rc-local.service 2>/dev/null || true
ls -l /dev/andordrvlx*

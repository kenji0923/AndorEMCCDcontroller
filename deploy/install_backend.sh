#!/bin/bash
# Installs the backend service. Run as root from the repository root after
# building: cmake --build build && sudo deploy/install_backend.sh build
set -euo pipefail

build_dir=${1:-build}
prefix=/usr/local

if [ "$(id -u)" -ne 0 ]; then
  echo "run as root" >&2
  exit 1
fi

# Only one backend can own the camera: stop a user-service install of the
# invoking user (deploy/install_backend_user.sh) and its tmpfiles entry.
if [ -n "${SUDO_USER:-}" ] &&
   systemctl --user -M "$SUDO_USER@" is-enabled --quiet andor-emccd-backend-user.service 2>/dev/null; then
  echo "disabling the user service of $SUDO_USER"
  systemctl --user -M "$SUDO_USER@" disable --now andor-emccd-backend-user.service
fi
rm -f /etc/tmpfiles.d/andor-emccd.conf

getent group andor >/dev/null || groupadd --system andor
getent passwd andor >/dev/null ||
  useradd --system --gid andor --home-dir /var/lib/andor-emccd \
          --shell /sbin/nologin andor

install -m 0755 "$build_dir/exec/andor_emccd_backend" "$prefix/bin/"
install -d -m 0755 /etc/andor-emccd
if [ ! -e /etc/andor-emccd/backend.json ]; then
  install -m 0644 deploy/backend.json /etc/andor-emccd/backend.json
fi
install -m 0644 deploy/andor-emccd-backend.service /etc/systemd/system/
install -m 0644 deploy/90-andor-emccd.rules /etc/udev/rules.d/
install -d -m 0755 "$prefix/share/doc/andor-emccd"
install -m 0644 doc/*.html "$prefix/share/doc/andor-emccd/"
udevadm control --reload-rules
udevadm trigger --subsystem-match=andordrvlx 2>/dev/null || true

if systemctl is-active --quiet firewalld; then
  zone=${AEC_FIREWALL_ZONE:-$(firewall-cmd --get-default-zone)}
  firewall-cmd --permanent --zone="$zone" --add-port=47100-47101/tcp
  firewall-cmd --reload
fi

systemctl daemon-reload
systemctl enable --now andor-emccd-backend.service
echo "Installed. Add GUI users on this host to the andor group for IPC:"
echo "  usermod -aG andor <user>"

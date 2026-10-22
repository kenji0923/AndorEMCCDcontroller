#!/bin/bash
# One-time root step for running the backend as a user service.
#   sudo deploy/prepare_user_service_root.sh [USER]     (default: $SUDO_USER)
# Creates the andor group and adds USER to it (device and IPC access), installs
# the udev rule for /dev/andordrvlx*, the tmpfiles entry for /run/andor-emccd,
# opens 47100-47101/tcp in firewalld and enables lingering so that the user
# service starts at boot without a login.
set -euo pipefail
cd "$(dirname "$0")/.."

user=${1:-${SUDO_USER:-}}
if [ "$(id -u)" -ne 0 ] || [ -z "$user" ]; then
  echo "usage: sudo $0 [USER]" >&2
  exit 1
fi

if systemctl is-enabled --quiet andor-emccd-backend.service 2>/dev/null ||
   systemctl is-active --quiet andor-emccd-backend.service; then
  echo "disabling the system service andor-emccd-backend (only one backend can own the camera)"
  systemctl disable --now andor-emccd-backend.service
fi

getent group andor >/dev/null || groupadd --system andor
usermod -aG andor "$user"

install -m 0644 deploy/90-andor-emccd.rules /etc/udev/rules.d/
udevadm control --reload-rules
udevadm trigger --subsystem-match=andordrvlx 2>/dev/null || true
for dev in /dev/andordrvlx*; do
  [ -e "$dev" ] && chgrp andor "$dev" && chmod 0660 "$dev"
done

install -m 0644 deploy/andor-emccd-tmpfiles.conf /etc/tmpfiles.d/andor-emccd.conf
systemd-tmpfiles --create /etc/tmpfiles.d/andor-emccd.conf

if systemctl is-active --quiet firewalld; then
  zone=${AEC_FIREWALL_ZONE:-$(firewall-cmd --get-default-zone)}
  firewall-cmd --permanent --zone="$zone" --add-port=47100-47101/tcp
  firewall-cmd --reload
fi

loginctl enable-linger "$user"
echo "Done. Log out and in again (new group), then as $user:"
echo "  deploy/install_backend_user.sh build"

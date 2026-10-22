#!/bin/bash
# Installs the backend as a user service (no root needed; run
# deploy/prepare_user_service_root.sh once first). From the repository root:
#   cmake --build build && deploy/install_backend_user.sh build
# AEC_CAMERA=sim writes a new configuration with the simulated camera.
set -euo pipefail
cd "$(dirname "$0")/.."

build_dir=${1:-build}
config_dir=$HOME/.config/andor-emccd
config=$config_dir/backend.json
unit=andor-emccd-backend-user.service

if [ "$(id -u)" -eq 0 ]; then
  echo "run as the user that owns the service, not root" >&2
  exit 1
fi
if ! id -nG | tr ' ' '\n' | grep -qx andor; then
  echo "warning: $(id -un) is not (yet) in the andor group in this session;" \
       "the camera device and /run/andor-emccd may not be accessible" >&2
fi

install -D -m 0755 "$build_dir/exec/andor_emccd_backend" "$HOME/.local/bin/andor_emccd_backend"
install -d -m 0755 "$config_dir" "$HOME/.local/share/doc/andor-emccd"
install -m 0644 doc/*.html "$HOME/.local/share/doc/andor-emccd/"
if [ ! -e "$config" ] || [ -n "${AEC_CAMERA:-}" ]; then
  # Same as the system configuration, with the settings kept in the home.
  sed -e "s|/var/lib/andor-emccd/settings.json|$HOME/.local/state/andor-emccd/settings.json|" \
      -e "s|\"camera\": \"andor\"|\"camera\": \"${AEC_CAMERA:-andor}\"|" \
      deploy/backend.json > "$config"
  echo "wrote $config"
else
  echo "kept existing $config"
fi
install -D -m 0644 "deploy/$unit" "$HOME/.config/systemd/user/$unit"

systemctl --user daemon-reload
systemctl --user enable "$unit"
systemctl --user restart "$unit"
sleep 2
systemctl --user --no-pager status "$unit" | head -12
if [ "$(loginctl show-user "$(id -un)" -p Linger --value 2>/dev/null)" != "yes" ]; then
  echo "note: lingering is off, so the service stops at logout and does not" \
       "start at boot (sudo loginctl enable-linger $(id -un))" >&2
fi

# AndorEMCCDcontroller

Controller for the Andor iXon3 EMCCD (DU-897E-CS0-#BV, CCI-24 PCI card).

- **Backend** (Linux, systemd service): controls the camera through Andor SDK2 and distributes settings, status, and images over ZeroMQ. A simulator (`--sim`) is available on PCs without a camera.
- **Frontend** (Qt6 GUI, Linux / Windows): live display with automatic or fixed scaling, zoom, and colormaps; camera settings; ROI statistics (sum, maximum, minimum, mean, and standard deviation) and formulas; UDP output for qtconsole; and TIFF / PNG / FITS export.
- **Python client** (`python/andor_emccd`): reads and changes settings, controls acquisition, and retrieves images from clients such as labscript BLACS workers.

See [doc/index.html](doc/index.html) for the design, installation instructions, and hardware verification results.

## Project layout

```
src/            C++17 / CMake sources (core: shared libraries, exec: backend and GUI, test: GoogleTest)
python/         Python client, labscript device, and test scripts (bench/)
deploy/         System/user systemd units, installers, udev rules, example configuration, and Grafana dashboards (grafana/)
tools/          Windows build scripts, camera PC setup scripts (camera_pc/), and utilities
doc/            HTML manual
```

## Communication

| Channel | Default endpoints | Content |
|---|---|---|
| control | `tcp://*:47100`, `ipc:///run/user/<uid>/aec/control` | Settings, status, and control ownership (JSON) |
| data | `tcp://*:47101`, `ipc:///run/user/<uid>/aec/data` | Raw images (header + pixel data) |

Clients on the same PC use IPC without going through the network. Only one client holds permission to change settings; other clients can still read them.

## Build

### Linux (AlmaLinux 9 / 10)

```bash
sudo dnf install -y epel-release && sudo dnf config-manager --set-enabled crb
sudo dnf install -y gcc-c++ cmake ninja-build git cppzmq-devel zeromq-devel spdlog-devel \
    qt6-qtbase-devel libtiff-devel libpng-devel cfitsio-devel
cmake -S src -B build -G Ninja -DANDOR_SDK_ROOT=/usr/local
cmake --build build
ctest --test-dir build --output-on-failure
```

If the Andor SDK (`atmcdLXd.h`, `libandor.so`) is unavailable, the backend is built with simulator support only. Obtain the SDK and driver from Andor; they are not included in this repository. See `tools/camera_pc/` and section 2 of doc/index.html for camera PC setup. The driver patch is not distributed here because it contains changes to Andor's driver source; section 2 describes the changes.

```bash
build/exec/andor_emccd_backend --sim                      # Try without a camera
build/exec/andor_emccd_backend --config deploy/backend.json
build/exec/gui/andor_emccd_gui --host <backend-host> --name <client-name>
```

### Run as a service (camera PC)

Either a system service or a user service can be used, but only one at a time. See section 2.3 of doc/index.html.

```bash
# System service: dedicated andor user, intended for a permanent camera PC
sudo AEC_FIREWALL_ZONE=internal deploy/install_backend.sh build
systemctl status andor-emccd-backend

# User service: runs as the login user; root is needed only for initial setup
sudo deploy/prepare_user_service_root.sh        # andor group, udev, /run/andor-emccd, firewall, linger
deploy/install_backend_user.sh build            # Run as the user after logging in again; also used for updates
systemctl --user status andor-emccd-backend-user
```

### Windows (frontend only)

Install Qt 6.8 (using aqtinstall) and vcpkg under `%USERPROFILE%\Opt`, then run `tools\build_windows.bat`. The distributable bundle is created in `build-win\deploy`; see section 10 of doc/index.html.

### GitHub Actions

`.github/workflows/build.yml` builds and tests each push, then uploads the following artifacts. Pushing a `v*` tag also attaches them to a GitHub release.

| Name | Content |
|---|---|
| `andor_emccd_gui-windows-x64` | Windows GUI (executable, Qt, and dependency DLLs) |
| `andor_emccd_gui-linux-x86_64` | Linux GUI AppImage (built on AlmaLinux 9; requires glibc 2.34 or later) |

The hardware backend requires the Andor SDK, so CI builds and tests the simulator backend. Build the hardware version on the camera PC.

## Python client

```bash
pip install -e python            # numpy, pyzmq
```

```python
from andor_emccd import AndorEMCCDClient
cam = AndorEMCCDClient("camera-pc", name="worker", acquire=True)   # Acquire control
cam.set_settings(exposure_time_s=0.01, em_gain_enabled=True, em_gain=50)
images, headers = cam.acquire(10)                              # numpy (n, h, w)
cam.release_control()
```

# AndorEMCCDRemote — labscript-suite device

BLACS controls the remote `andor_emccd_backend` through the Python client.
The tab retains the IMAQdx image viewer and adds the FanLab standard settings
panel: shared committing spinboxes, unit selection, bulk Enter commits,
readback reconciliation, and `FrontPanelPersistence`.

## Manual controls

- **Connect** opens a read-only connection and reads the current backend values.
  Worker startup and restoring the panel do not take control or change settings.
- **Request control** / **Release control** manage the backend control right.
  The holder and camera temperature appear in the status line.
  A connected tab is read-only until it owns control. **Take control** explicitly
  transfers control from the displayed holder (for example the native GUI).
  The worker is named `BLACS:<device>`; ownership is determined by its unique
  client ID, so matching display names cannot collide.
- The settings pages cover the same 40 fields as the native Qt SettingsPanel:
  acquisition, readout, EM gain, image area, cooling, and shutter.
- Numeric entries commit on Enter or a step; a commit sends pending fields in
  one transaction. Escape reverts edits. Readbacks preserve focused/pending
  edits; inactive edits expire after 60 seconds. Failed writes remain visible.
- **Snap**, **Continuous**, and **Stop** retain the camera preview functions.
  Stop preview before editing settings. The Attributes dialog remains available
  for copying settings into a connection table.
- Settings are persisted in BLACS and each shot's front panel. Reconnecting
  reads the backend's state rather than programming saved panel values.

The connection-table properties `poll_interval_ms=100`,
`apply_timeout_ms=3000`, and `edit_expiry_ms=60000` control panel timing.

## Software-triggered shots

Only software triggering is implemented for buffered shots. There is no
trigger parent, digital connection, or trigger pulse. Existing externally
triggered connection tables and old compiled shots must be updated/recompiled.
The standalone `AndorEMCCDRemoteCamera` Python interface still supports external
acquisition for clients outside this labscript device.

```python
from user_devices.fanlab_devices.AndorEMCCDRemote.labscript_devices import AndorEMCCDRemote

# Keep the experiment's normal master pseudoclock in the connection table.
AndorEMCCDRemote(
    name="emccd",
    host="camera-pc",
    orientation="side",
    camera_attributes={
        "trigger_mode": "software",
        "exposure_time_s": 1e-3,
        "em_gain_enabled": True,
        "em_gain": 100,
        "hbin": 1, "vbin": 1,
    },
    minimum_recovery_time=0.05,  # minimum start-to-start spacing, seconds
    stop_acquisition_timeout=10.0,
    force_control=False,
)
```

```python
start()
emccd.expose(0.1000, "fluorescence", frametype="atoms", exposure_time=1e-3)
emccd.expose(0.2000, "fluorescence", frametype="background", exposure_time=1e-3)
stop(0.3)
```

`exposure_time` is in seconds and defaults to `camera_attributes['exposure_time_s']`.
All exposures within a shot must use the same duration. The SDK only accepts
software triggers in continuous (run-till-abort) mode; the worker configures
that mode automatically and stops after receiving the requested frames.

**Every start time must be a non-negative multiple of 0.1 ms (100 µs).**
For example, `0.1000` and `0.1001` seconds are valid; `0.10001` is rejected.
Values are never silently rounded. Validation occurs both during compilation
and before buffered acquisition, so edited/old HDF5 files cannot bypass it.
Exposure duration itself does not have this grid restriction. Overlapping
exposures, insufficient camera recovery time, exposures extending past `stop`,
and shots containing labscript waits are also rejected.

The time origin is **receipt of BLACS's `science_starting` notification**,
just before BLACS asks the master pseudoclock to start. The tab registers this
callback automatically; a fresh event token is used for every attempt, including
repeats. A Python thread waits on absolute monotonic deadlines and sends
software-trigger commands. It never starts the schedule during buffered
programming. The master must start immediately when BLACS requests it; an
externally delayed master start is not supported.

The 100 µs grid is an input restriction, **not a 100 µs accuracy guarantee**.
OS scheduling, event delivery, master start, network transport, and camera
latency contribute jitter/offset. The saved `trigger_sent_times_s` records
command dispatch times relative to the notification, not measured exposure
start times.

## Control and shot lifecycle

`transition_to_buffered` always requests control, even for empty shots or
unchanged/repeated settings. If another GUI holds control, the transition fails
with the holder's name. Release it there first, or explicitly configure
`force_control=True` to permit takeover for this worker.

The worker snapshots the current manual settings, applies the full shot settings,
checks actual camera timings, and starts acquisition. Triggering and receiving
use separate threads/sockets, so waiting for an image does not delay the next
scheduled trigger. Abort cancels pending triggers and stops acquisition.
Background exceptions and missing frames cause the shot to fail.

Images stream into a `pythonlib.data_saver.DataSaver` sidecar. The main shot
file stays closed during acquisition. `transition_to_manual` joins the thread,
merges into `/data/<device>/camera/images`, and exposes the same data through
`/images/<orientation or device>/<name>/<frametype>` for IMAQdx compatibility.
Requested/dispatch times, camera settings, actual timings, and frame headers
are saved with the images. Abort deletes the sidecar without merging. The
worker restores pre-shot manual settings if it still owns control.

## Installation

Install the client with `pip install /path/to/AndorEMCCDcontroller/python`.
The tab also needs the lab's existing `user_devices.fanlab_devices.utils` and
`pythonlib.data_saver` / `pythonlib.online_monitor` modules in the DAQ environment.

Copy this device directory into
`~/labscript-suite/userlib/user_devices/fanlab_devices/AndorEMCCDRemote/`,
then restart its BLACS tab and recompile the connection table. The registration
file targets that FanLab package. If installed directly under `user_devices`,
adjust `register_classes.py` accordingly. Do not add the repository's whole
`python/` source directory to labscript's import path: it can shadow an editable
installation of the real `labscript_devices` package.

This repository copy does not automatically replace a separately installed
FanLab copy. Backend ports default to 47100/47101; local IPC is selected when
the sockets exist in `ipc_dir` (default `/run/andor-emccd`).

## Verification

Use the DAQ Python environment. `READTHEDOCS=1` disables HDF5's network zlock
hook for these isolated local-file tests only; do not set it for normal BLACS.
Integration tests refuse to operate on a non-simulated backend.

```bash
READTHEDOCS=1 QT_QPA_PLATFORM=offscreen \
AEC_TEST_HOST=127.0.0.1 AEC_TEST_CONTROL_PORT=47550 \
AEC_TEST_DATA_PORT=47551 AEC_TEST_IPC_DIR=/tmp/aec-blacs-test \
~/.pyenv/daq/bin/python -m pytest -q \
    python/tests/test_labscript_remote.py python/tests/test_remote_camera.py
```

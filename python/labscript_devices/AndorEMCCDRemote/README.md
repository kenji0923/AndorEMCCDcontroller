# AndorEMCCDRemote — labscript-suite device

BLACS controls the remote `andor_emccd_backend` through the Python client.
The tab provides the FanLab standard settings panel: shared committing
spinboxes, unit selection, bulk Enter commits,
readback reconciliation, and `FrontPanelPersistence`.
Camera images are displayed in the standalone EMCCD GUI. BLACS handles
settings/control ownership and buffered shot acquisition/saving without an
image viewer or image-receiver connection.

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
exposures, insufficient camera recovery time, and exposures extending past `stop`
are also rejected.

Labscript waits are **ignored by the software camera scheduler**. Trigger times
remain elapsed wall-clock times from the software start notification; a wait
does not pause, delay, or restart the camera schedule. For example, an exposure
at 10 s still triggers 10 s after notification even if the hardware pseudoclock
is waiting. Other devices retain their normal wait behavior. The shot and saved
camera data record `wait_policy="ignore"`.

The time origin is **receipt of BLACS's `science_starting` notification**,
just before BLACS asks the master pseudoclock to start. The tab registers this
callback automatically; a fresh event token is used for every attempt, including
repeats. A Python thread waits on absolute `time.perf_counter()` deadlines and sends
software-trigger commands. It never starts the schedule during buffered
programming. The master must start immediately when BLACS requests it; an
externally delayed master start is not supported.

The worker saves requested, dispatch, and command-acknowledgement times, plus
the counter implementation and resolution, with the images. On Windows Python
3.10, `perf_counter` uses QueryPerformanceCounter; `monotonic` uses the much
coarser GetTickCount64 clock. Dispatch and acknowledgement timestamps measure
software activity, not the camera's physical exposure edges.

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
After a successful shot, each frame also gets an 8-bit RGB grayscale preview at
`/data/<device>/camera/previews/frame_0000` (and subsequent frame numbers).
These use the same HDF5 IMAGE-class attributes as the scope screenshots so
HDFView and compatible image viewers can render them directly. Repeated
name/frametype exposures each get their own preview, even when their raw data
is stored as a stack.

Matching PNGs are written beside the shot in
`<device>_png/<shot_name>_frame0000.png`. PNG export requires Pillow in the
BLACS worker environment. Each preview independently maps its raw minimum and
maximum to black and white; constant frames are black. The source dataset,
frame index, exposure information, display range, and relative PNG filename
are recorded on the preview. Raw image values and dtype are preserved. Preview
or PNG export failures log warnings without invalidating the saved raw shot.

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

## GUI display settings (v0.3.0)

Buffered shots force camera `count_convert_mode=0` to preserve raw ADU counts.
Native GUIs connected to the same backend publish independent profiles without
acquiring camera control. At buffered transition, the worker snapshots all known
profiles and re-reads SDK calibration for the shot's actual readout/gain settings.
The UTF-8 JSON datasets under `data/<device>/camera/images` are:

- `gui_settings_snapshot`: GUI profiles, host/client, update time, original GUI
  calibration, and the calibration resolved for the shot.
- `frame_display_metadata`: one JSON record per frame, with each profile's BG
  ROI mean/count and the derived offset/scale (raw pixels are unchanged).

GUI settings are captured at execution, not compilation. Profiles are stored in
backend memory; start/connect the GUI after a backend restart. Saved timestamps
show when a closed or disconnected GUI last published its profile. Older
backends record an explicit unavailable status and still acquire raw images.

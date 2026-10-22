# AndorEMCCDRemote — labscript-suite device

A labscript-suite camera device for the Andor iXon3 EMCCD that is driven by
the `andor_emccd_backend` service (this repository), possibly running on
another machine. BLACS talks to the backend over ZeroMQ through the
`andor_emccd` Python client. The device follows the
`IMAQdxCamera` pattern (like `PylonCamera`, `SpinnakerCamera`,
`AndorSolis`): same BLACS tab (snap / continuous / attributes dialog), same
HDF5 layout (`images/<orientation or name>/<name>/<frametype>`).

| File | Content |
| --- | --- |
| `labscript_devices.py` | `AndorEMCCDRemote(IMAQdxCamera)`; extra connection-table args `host`, `control_port`, `data_port`, `ipc_dir` |
| `blacs_tabs.py` | `AndorEMCCDRemoteTab(IMAQdxCameraTab)`; passes the backend address to the worker |
| `blacs_workers.py` | `AndorEMCCDRemoteCamera` (camera interface over `AndorEMCCDClient`) and `AndorEMCCDRemoteWorker(IMAQdxCameraWorker)` |
| `register_classes.py` | registers the BLACS tab |

## How a shot runs

* `transition_to_buffered`: `camera_attributes` are sent with `set_settings`
  (only changed ones, IMAQdx "smart programming"), then the backend is
  **armed** for a kinetic series with as many frames as `expose()` calls
  (`arm(n, settings=<all attributes programmed so far>)`). Note `arm` aborts
  any run in progress on the backend (e.g. one started from the Qt GUI).
* During the shot an acquisition thread calls `wait(run_id, n_frames=k+1)`
  with a short timeout and `fetch`es the new frames, so the image list
  fills as frames arrive.
* `transition_to_manual`: waits up to `stop_acquisition_timeout`, saves the
  images, calls `stop` (no-op if the run completed). On abort `stop` is sent
  immediately (on a separate socket, so it is not queued behind the wait).
* Manual mode: *Snap* arms a single frame (and sends a software trigger if
  `trigger_mode == "software"`); *Continuous* starts a `continuous` run and
  shows the latest frames (`fetch_latest`).

Attributes are the backend settings keys (`exposure_time_s`, `em_gain`,
`trigger_mode`, `hbin`, `vstart`, ...; see the main README / `get_settings`).
The BLACS *Attributes* dialog lists them in three visibility levels and the
selected level is saved with every shot.

## Installation

1. Install the client into the Python environment that runs BLACS
   (and lyse, if it needs it):

   ```bash
   pip install /path/to/AndorEMCCDcontroller/python   # numpy, pyzmq
   ```

2. Copy (or symlink) this folder into your labscript `user_devices` package
   (`~/labscript-suite/userlib/user_devices/` by default, it must contain an
   `__init__.py`):

   ```bash
   cp -r /path/to/AndorEMCCDcontroller/python/labscript_devices/AndorEMCCDRemote \
         ~/labscript-suite/userlib/user_devices/
   ```

   `labconfig` must list the folder, e.g.
   `[DEFAULT] user_devices = user_devices` (the default). If you place it
   under a different package name, edit the import path in
   `register_classes.py`.

3. Make sure BLACS can reach the backend: on the backend host open TCP
   ports 47100 (control) and 47101 (data) in the firewall. When BLACS runs on
   the backend host itself, the IPC sockets in `ipc_dir` are used
   automatically.

Do not put `python/` of this repository on `sys.path` of labscript as a
whole folder *and* expect `labscript_devices` to resolve to it — the
`python/labscript_devices/` folder here is only a source location (it has no
`__init__.py`, so the real `labscript_devices` package always wins).

## Example connection table

```python
from labscript import start, stop
from labscript_devices.PulseBlasterUSB import PulseBlasterUSB
from user_devices.AndorEMCCDRemote.labscript_devices import AndorEMCCDRemote

PulseBlasterUSB(name="pb")

AndorEMCCDRemote(
    name="emccd",
    parent_device=pb.direct_outputs,
    connection="flag 0",
    host="camera-pc",             # machine running andor_emccd_backend
    control_port=47100,
    data_port=47101,
    trigger_duration=10e-6,
    minimum_recovery_time=30e-3,  # >= kinetic cycle (readout) time
    orientation="side",
    camera_attributes={
        "trigger_mode": "external",
        "exposure_time_s": 1e-3,
        "em_gain_enabled": True,   # EM gain is off unless enabled
        "em_gain": 100,
        "output_amplifier": 0,
        "frame_transfer": False,
        "hbin": 1, "vbin": 1,
        "hstart": 1, "hend": 0, "vstart": 1, "vend": 0,
    },
    manual_mode_camera_attributes={"trigger_mode": "software"},
    stop_acquisition_timeout=10.0,
)

if __name__ == "__main__":
    start()
    stop(1)
```

In an experiment script:

```python
start()
t = 0.1
emccd.expose(t, "fluorescence", frametype="atoms")
emccd.expose(t + 0.05, "fluorescence", frametype="background")
stop(t + 0.2)
```

## Testing without labscript

`AndorEMCCDRemoteCamera` has no labscript dependency. With a simulated
backend running:

```bash
python3 -m pytest -v python/tests/test_remote_camera.py
```

## Settings control right

The backend lets only one client change settings or start/stop
acquisitions at a time. The worker requests this right before every
changing operation (shown to other clients as `labscript@<ip>`). If a GUI
holds it, the shot fails with `ControlDeniedError` naming the holder;
release control in the GUI, or set `force_control=True` in the worker
arguments to take it over. Reading (fetching frames, status) needs no
control.

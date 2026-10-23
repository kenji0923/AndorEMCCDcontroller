"""BLACS worker for the Andor iXon EMCCD backend service.

``AndorEMCCDRemoteCamera`` implements the camera interface expected by
``IMAQdxCameraWorker`` (set_attributes, get_attribute, snap,
configure_acquisition, grab, grab_multiple, stop/abort_acquisition, close)
on top of :class:`andor_emccd.AndorEMCCDClient`. It has no labscript
dependency, so it can be used and tested without BLACS; the worker class is
only defined when labscript_devices is importable.
"""

from __future__ import annotations

import json
import sys
import threading
import time
from typing import Any, Dict, List, Optional

import numpy as np

from andor_emccd import (AndorEMCCDClient, AndorEMCCDError,
                         ControlDeniedError, NotControllerError)
from andor_emccd.client import DEFAULT_IPC_DIR
from .timing import validate_exposures

try:
    from labscript_devices.IMAQdxCamera.blacs_workers import (
        IMAQdxCameraWorker, MockCamera)
except ImportError:  # labscript/BLACS not installed: camera class only.
    IMAQdxCameraWorker = None
    MockCamera = None

# Attribute names per IMAQdx visibility level (each level includes the
# previous one; "advanced" is everything the backend reports).
_SIMPLE_ATTRIBUTES = (
    "acquisition_mode", "trigger_mode", "exposure_time_s",
    "em_gain_enabled", "em_gain",
    "hbin", "vbin", "hstart", "hend", "vstart", "vend",
    "cooler_on", "target_temperature_c", "shutter_mode",
)
_INTERMEDIATE_ATTRIBUTES = _SIMPLE_ATTRIBUTES + (
    "fast_external_trigger", "trigger_invert", "number_kinetics",
    "kinetic_cycle_time_s", "number_accumulations",
    "accumulation_cycle_time_s", "frame_transfer", "output_amplifier",
    "hs_speed_index", "vs_speed_index", "vs_amplitude", "preamp_gain_index",
    "em_gain_mode", "isolated_crop", "crop_width", "crop_height", "fan_mode",
)


def _is_wait_timeout(error: AndorEMCCDError) -> bool:
    # Backends since protocol 1 send code "timeout"; keep the text check
    # as a fallback.
    return (getattr(error, "code", None) == "timeout"
            or str(error).startswith("timeout waiting"))


class AndorEMCCDRemoteCamera:
    """Camera interface (IMAQdx style) backed by the EMCCD backend service.

    Attributes are backend settings (see ``get_settings``). Buffered shots
    use a continuous run for software triggers; external-trigger callers
    of this standalone interface can still use ``arm`` for a kinetic series.

    Note: ``arm`` and ``start`` abort any run in progress on the backend.
    """

    def __init__(self, host: str = "localhost", control_port: int = 47100,
                 data_port: int = 47101, ipc: Optional[bool] = None,
                 ipc_dir: str = DEFAULT_IPC_DIR, timeout_s: float = 10.0,
                 poll_interval_s: float = 0.5,
                 snap_timeout_s: float = 10.0,
                 fetch_chunk: int = 32,
                 name: str = "labscript",
                 force_control: bool = False):
        kwargs = dict(host=host, control_port=control_port,
                      data_port=data_port, ipc=ipc, ipc_dir=ipc_dir,
                      timeout_s=timeout_s, name=name)
        # Commands (set/stop/abort) go through _client; the acquisition
        # thread blocks in wait/fetch on its own socket so that an abort
        # from the worker thread is never queued behind a pending wait.
        self._client = AndorEMCCDClient(**kwargs)
        self._acq_client = AndorEMCCDClient(**kwargs)
        # Both sockets act as one client for the control right.
        self._acq_client.client_id = self._client.client_id
        self._force_control = force_control
        print(f"Connecting to EMCCD backend at {self._client.control_endpoint}")
        self.info = self._client.get_info()
        print(f"Connected: {self.info.get('model')} "
              f"(serial {self.info.get('serial')})")
        self.poll_interval_s = poll_interval_s
        self.snap_timeout_s = snap_timeout_s
        self.fetch_chunk = max(1, int(fetch_chunk))
        self.attributes: Dict[str, Any] = {}
        self.exception_on_failed_shot = True
        self.last_headers: List[Dict[str, Any]] = []
        self._abort_acquisition = False
        self._run_id: Optional[int] = None
        self._continuous = False
        self._software = False
        self._last_seq: Optional[int] = None
        self._last_index = -1
        self.last_run = None

    def _ensure_control(self) -> None:
        """Takes the settings control right (no-op if already held).

        Raises ControlDeniedError naming the holder (e.g. a GUI) unless
        force_control was set in the connection table.
        """
        self._client.acquire_control(force=self._force_control)

    # ----- attributes -----

    def set_attributes(self, attr_dict: Dict[str, Any]) -> None:
        """Programs several settings in one backend transaction."""
        if not attr_dict:
            return
        self._ensure_control()
        reply = self._client.set_settings(dict(attr_dict))
        self.attributes.update(attr_dict)
        for warning in reply.get("warnings", []) or []:
            print(f"Warning: {warning}", file=sys.stderr)

    def set_attribute(self, name: str, value: Any) -> None:
        self.set_attributes({name: value})

    def get_attribute_names(self, visibility_level: str,
                            writeable_only: bool = True) -> List[str]:
        names = list(self._client.get_settings().keys())
        level = (visibility_level or "advanced").lower()
        if level == "simple":
            return [n for n in _SIMPLE_ATTRIBUTES if n in names]
        if level == "intermediate":
            return [n for n in _INTERMEDIATE_ATTRIBUTES if n in names]
        return names

    def get_attribute(self, name: str) -> Any:
        settings = self._client.get_settings()
        if name not in settings:
            raise KeyError(f"unknown attribute {name!r}")
        return settings[name]

    def get_attributes(self, visibility_level: str) -> Dict[str, Any]:
        """Returns {name: value} for a visibility level (one request)."""
        settings = self._client.get_settings()
        names = self.get_attribute_names(visibility_level)
        result = {}
        for name in names:
            value = settings[name]
            # HDF5 attributes cannot hold dicts/lists of mixed types.
            if isinstance(value, (dict, list)):
                value = json.dumps(value)
            result[name] = value
        return result

    # ----- acquisition -----

    def snap(self) -> np.ndarray:
        """Acquires a single frame with the current settings."""
        self._ensure_control()
        software = (self._client.get_settings().get("trigger_mode")
                    == "software")
        if software:
            # The SDK accepts software triggers only in run-till-abort mode.
            run = self._client.start({"acquisition_mode": "continuous"})
            self._client.software_trigger()
        else:
            run = self._client.arm(1)
        run_id = run["run_id"]
        try:
            self._acq_client.wait(run_id, n_frames=1,
                                  timeout_s=self.snap_timeout_s)
        except (AndorEMCCDError, TimeoutError):
            self._stop()
            raise
        if software:
            self._stop()
        images, headers = self._acq_client.fetch(run_id, start=0, count=1)
        self.last_headers = headers
        return images[0]

    def configure_acquisition(self, continuous: bool = True,
                              bufferCount: int = 5) -> None:
        """Start continuous/software acquisition, or an external kinetic series."""
        self._abort_acquisition = False
        self._last_seq = None
        self._last_index = -1
        self.last_headers = []
        software = self.attributes.get(
            "trigger_mode", self._client.get_settings().get("trigger_mode")) == "software"
        if continuous or software:
            self._ensure_control()
            self._client.stop()
            run = self._client.start({**self.attributes, "acquisition_mode": "continuous"})
        else:
            # Pass every attribute programmed so far, so the shot does not
            # depend on changes made to the backend from elsewhere (GUI).
            self._ensure_control()
            run = self._client.arm(int(bufferCount), dict(self.attributes))
        for warning in run.get("warnings", []) or []:
            print(f"Warning: {warning}", file=sys.stderr)
        self._run_id = run["run_id"]
        self.last_run = run
        self._continuous = continuous
        self._software = (self._client.get_settings().get("trigger_mode")
                          == "software")

    def grab(self, waitForNextBuffer: bool = True) -> np.ndarray:
        """Returns the next frame of the continuous run."""
        if self._run_id is None:
            raise RuntimeError("configure_acquisition was not called")
        if self._software:
            self._client.software_trigger()
        while not self._abort_acquisition:
            image, header = self._acq_client.fetch_latest(self._last_seq)
            if image is not None and header["run_id"] == self._run_id:
                self._last_seq = header["seq"]
                self._last_index = header["index"]
                self.last_headers = [header]
                return image
            n_next = self._last_index + 2
            try:
                run = self._acq_client.wait(self._run_id, n_frames=n_next,
                                            timeout_s=self.poll_interval_s)
            except AndorEMCCDError as e:
                if not _is_wait_timeout(e):
                    raise
                continue
            if run["complete"] and run["frames"] < n_next:
                raise RuntimeError(f"run {self._run_id} ended")
        raise RuntimeError("acquisition aborted")

    def grab_multiple(self, n_images: int, images: list,
                      waitForNextBuffer: bool = True) -> None:
        """Appends the n_images frames of the armed run to images.

        Frames are fetched as they arrive, so ``images`` fills during the
        shot. Returns early on abort or if the run ends short.
        """
        run_id = self._run_id
        if run_id is None:
            raise RuntimeError("configure_acquisition was not called")
        print(f"Attempting to grab {n_images} images (run {run_id}).")
        while len(images) < n_images:
            if self._abort_acquisition:
                print("Abort during acquisition.")
                self._abort_acquisition = False
                return
            try:
                run = self._acq_client.wait(run_id, n_frames=len(images) + 1,
                                            timeout_s=self.poll_interval_s)
            except AndorEMCCDError as e:
                if not _is_wait_timeout(e):
                    raise
                print(".", end="", flush=True)
                continue
            while len(images) < run["frames"]:
                count = min(run["frames"] - len(images), self.fetch_chunk)
                frames, headers = self._acq_client.fetch(
                    run_id, start=len(images), count=count)
                images.extend(list(frames))
                self.last_headers.extend(headers)
                print(f"Got image(s) {len(images) - len(headers) + 1}-"
                      f"{len(images)} of {n_images}.")
            if run["complete"] and len(images) < n_images:
                msg = (f"run {run_id} ended after {len(images)} of "
                       f"{n_images} frames"
                       + (" (aborted)" if run.get("aborted") else ""))
                if self._abort_acquisition:
                    self._abort_acquisition = False
                    print(msg)
                    return
                if self.exception_on_failed_shot:
                    raise RuntimeError(msg)
                print(msg, file=sys.stderr)
                return
        print(f"Got {len(images)} of {n_images} images.")

    def _stop(self) -> None:
        """Stops our run; if another client took control it already did."""
        try:
            self._client.stop()
        except NotControllerError:
            pass

    def stop_acquisition(self) -> None:
        self._stop()
        self._run_id = None

    def abort_acquisition(self) -> None:
        self._abort_acquisition = True
        self._stop()

    def close(self) -> None:
        self._acq_client.close()
        self._client.close()


class _FrameSink:
    """List-like sink used by grab_multiple; only the latest image stays in RAM."""

    def __init__(self, saver, exposures):
        self.saver = saver
        self.exposures = exposures
        self.count = 0
        self.last_image = None
        self.repeated = {}
        for row in exposures:
            key = (row["name"], row["frametype"])
            self.repeated[key] = self.repeated.get(key, 0) + 1

    def __len__(self):
        return self.count

    def extend(self, images):
        for image in images:
            file = self.saver.get_open_file()
            if file is None:
                raise RuntimeError("Camera sidecar is no longer writable")
            row = self.exposures[self.count]
            group = file.require_group(row["name"])
            label = row["frametype"]
            if self.repeated[(row["name"], label)] == 1:
                dataset = group.create_dataset(label, data=image, compression="gzip")
            else:
                if label not in group:
                    group.create_dataset(label, shape=(0,) + image.shape,
                                         maxshape=(None,) + image.shape,
                                         dtype=image.dtype, compression="gzip")
                dataset = group[label]
                dataset.resize(dataset.shape[0] + 1, axis=0)
                dataset[-1] = image
            dataset.attrs.update(CLASS=np.bytes_("IMAGE"),
                                 IMAGE_VERSION=np.bytes_("1.2"),
                                 IMAGE_SUBCLASS=np.bytes_("IMAGE_GRAYSCALE"),
                                 IMAGE_WHITE_IS_ZERO=np.uint8(0))
            self.count += 1
            self.last_image = image
            file.flush()


if IMAQdxCameraWorker is not None:

    class AndorEMCCDRemoteWorker(IMAQdxCameraWorker):
        """Shared backend connection with a cancellable software-shot scheduler."""

        interface_class = AndorEMCCDRemoteCamera

        def init(self):
            import zmq
            from labscript_utils.ls_zprocess import Context, Event
            from pythonlib.online_monitor.status import StatusMetrics
            from .shot_start import EVENT_NAME

            # Do not acquire control, change settings, or connect at startup.
            self.camera = None
            self.smart_cache = {}
            self.continuous_stop = threading.Event()
            self.continuous_thread = None
            self.continuous_dt = None
            self.acquisition_thread = None
            self._cancel = threading.Event()
            self._ready = threading.Event()
            self._shot_error = None
            self._saver = None
            self._manual_settings = None
            self._start_token = None
            self._start_event = Event(EVENT_NAME, role="wait")
            self.h5_filepath = None
            self.image_socket = Context().socket(zmq.REQ)
            self.image_socket.connect(f"tcp://{self.parent_host}:{self.image_receiver_port}")
            self._metrics = StatusMetrics("emccd_camera", tags={"device_name": self.device_name},
                                          logger=self.logger)

        def get_camera(self):
            if self.mock:
                camera = MockCamera()
                camera.set_attributes(self.camera_attributes)
                return camera
            return self.interface_class(
                host=self.host, control_port=self.control_port,
                data_port=self.data_port, ipc_dir=self.ipc_dir,
                name=f"BLACS:{self.device_name}",
                force_control=getattr(self, "force_control", False))

        def connect_to_device(self):
            if self.camera is None:
                self.camera = self.get_camera()
            return self.read_panel()

        def disconnect_from_device(self):
            if self.continuous_thread is not None:
                self.stop_continuous()
            if self.camera is not None:
                self.camera.close()
                self.camera = None
            return {"connected": False}

        def read_panel(self):
            if self.camera is None:
                return {"connected": False}
            if self.mock:
                return dict(connected=True, mine=True, control={"holder": "mock"},
                            settings=dict(self.camera.attributes), info={}, status={})
            client = self.camera._client
            status = client.get_status()
            self._metrics.send({key: value for key, value in status.items()
                                if isinstance(value, (float, int))})
            control = client.get_control()
            return dict(connected=True, settings=client.get_settings(),
                        info=client.get_info(), status=status, control=control,
                        mine=bool(control and control.get("holder_id") == client.client_id))

        def panel_action(self, action, values=None):
            """Return expected transport/control errors to the panel, not BLACS fatal state."""
            try:
                if action == "connect":
                    return self.connect_to_device()
                if action == "disconnect":
                    return self.disconnect_from_device()
                if action == "read":
                    return self.read_panel()
                if self.camera is None:
                    raise RuntimeError("Connect to the backend first")
                if action in ("request", "take_control"):
                    if not self.mock:
                        self.camera._client.acquire_control(force=action == "take_control")
                elif action == "release":
                    if self.continuous_thread is not None:
                        self.stop_continuous()
                    if not self.mock:
                        self.camera._client.release_control()
                elif action == "settings":
                    if self.continuous_thread is not None:
                        raise RuntimeError("Stop continuous preview before changing settings")
                    if not self.mock and not self.camera._client.has_control():
                        raise RuntimeError("Request control before changing settings")
                    self.camera.set_attributes(values)
                return self.read_panel()
            except ControlDeniedError as error:
                return {**self.read_panel(), "action_error": str(error)}
            except Exception as error:
                return {"error": str(error)}

        def get_attributes_as_dict(self, visibility_level):
            if self.camera is None:
                return {}
            if isinstance(self.camera, AndorEMCCDRemoteCamera):
                return self.camera.get_attributes(visibility_level)
            return super().get_attributes_as_dict(visibility_level)

        def snap(self):
            if self.camera is None:
                raise RuntimeError("Connect and request control before snapping")
            return super().snap()

        def start_continuous(self, dt):
            if self.camera is None:
                raise RuntimeError("Connect and request control before continuous preview")
            # Read back the GUI's current settings, not a cache from an earlier shot.
            if not self.mock:
                self.camera.attributes = self.camera._client.get_settings()
            return super().start_continuous(dt)

        def stop_continuous(self, pause=False):
            self.continuous_stop.set()
            self.camera.abort_acquisition()  # unblock external-trigger waits as well
            self.continuous_thread.join()
            self.continuous_thread = None
            self.continuous_stop.clear()
            self.camera.stop_acquisition()
            if not pause:
                self.continuous_dt = None

        def continuous_loop(self, dt):
            try:
                super().continuous_loop(dt)
            except Exception:
                if not self.continuous_stop.is_set():
                    self.logger.exception("Continuous camera preview failed")

        def prepare_shot_start(self, token):
            self._start_token = token

        def transition_to_buffered(self, device_name, h5_filepath, initial_values, fresh):
            import labscript_utils.h5_lock  # noqa: F401
            import h5py
            import labscript_utils.properties
            from labscript_utils.shared_drive import path_to_local
            from pythonlib.data_saver import DataSaver

            # Request control on EVERY transition, including empty shots and repeats.
            self.connect_to_device()
            if not self.mock:
                self.camera._ensure_control()
            self._cancel.clear()
            self._ready.clear()
            self._shot_error = None
            self._sink = None
            self._trigger_times = []
            if self.continuous_thread is not None:
                self.stop_continuous(pause=True)
            try:
                path = path_to_local(h5_filepath) if getattr(self, "is_remote", False) else h5_filepath
                with h5py.File(path, "r") as file:
                    group = file["devices"][device_name]
                    props = labscript_utils.properties.get(file, device_name, "device_properties")
                    if group.attrs.get("trigger_method") != "software":
                        raise ValueError("Recompile this shot for software triggering")
                    if "waits" in file and len(file["waits"]):
                        raise ValueError("Software camera shots do not support labscript waits")
                    rows = group["EXPOSURES"][:] if "EXPOSURES" in group else []
                    self.exposures = [dict(t=float(row["t"]),
                        exposure_time_s=float(row["exposure_time_s"]),
                        name=row["name"].decode() if isinstance(row["name"], bytes) else row["name"],
                        frametype=row["frametype"].decode() if isinstance(row["frametype"], bytes) else row["frametype"])
                        for row in rows]
                    self.exposures = validate_exposures(self.exposures,
                        props.get("minimum_recovery_time", 0), group.attrs["stop_time"])
                    stop_time = float(group.attrs["stop_time"])
                self.h5_filepath = path
                self.stop_acquisition_timeout = props["stop_acquisition_timeout"]
                if not self.exposures:
                    return {}
                if not self._start_token:
                    raise RuntimeError("BLACS science-start notification was not registered")
                settings = dict(props["camera_attributes"])
                if settings.get("trigger_mode") != "software":
                    raise ValueError("Only software-triggered buffered shots are supported")
                self._manual_settings = (dict(self.camera.attributes) if self.mock
                                         else self.camera._client.get_settings())
                settings.update(trigger_mode="software", acquisition_mode="continuous",
                                exposure_time_s=self.exposures[0]["exposure_time_s"])
                self.camera.attributes = {}
                self.camera.set_attributes(settings)
                self.camera.configure_acquisition(continuous=False, bufferCount=len(self.exposures))
                if not self.mock:
                    timings = self.camera.last_run["timings"]
                    recovery = max(float(timings["kinetic_s"]),
                                   float(timings["exposure_s"]) + float(timings["readout_s"]))
                    validate_exposures(self.exposures, recovery, stop_time)
                    if self.exposures[-1]["t"] + timings["exposure_s"] > stop_time + 1e-12:
                        raise ValueError("Actual camera exposure extends beyond the shot stop time")
                    window = props.get("staircase_acquire_s")
                    if window is not None and timings["exposure_s"] > float(window) + 1e-6:
                        raise ValueError("Actual exposure exceeds staircase acquisition window")
                else:
                    timings = {}
                self._saver = DataSaver(path, device_name=device_name,
                                        acquisition_type="camera", data_label="images")
                self._saver.add_attribute("camera", device_name)
                self._saver.add_attribute("trigger_method", "software")
                self._saver.add_attribute("timing_origin", "receipt of BLACS science_starting")
                self._saver.add_attribute("timings", json.dumps(timings))
                self._saver.add_attribute("camera_settings", json.dumps(
                    settings if self.mock else self.camera._client.get_settings()))
                level = props.get("saved_attribute_visibility_level")
                if level is not None:
                    for key, value in self.get_attributes_as_dict(level).items():
                        self._saver.add_attribute(key, value)
                self.acquisition_thread = threading.Thread(target=self._acquire_shot,
                                                           name="emccd-shot", daemon=True)
                self.acquisition_thread.start()
                if not self._ready.wait(10):
                    raise RuntimeError("Camera acquisition thread did not become ready")
                if self._shot_error is not None:
                    raise RuntimeError("Camera acquisition setup failed") from self._shot_error
                return {}
            except Exception:
                self.abort()
                raise

        def _send_triggers(self, origin):
            try:
                for row in self.exposures:
                    if self._cancel.wait(max(0, origin + row["t"] - time.monotonic())):
                        return
                    self._trigger_times.append(time.monotonic() - origin)
                    self.camera._client.software_trigger()
            except Exception as error:
                self._shot_error = error
                self.camera.abort_acquisition()

        def _acquire_shot(self):
            import h5py
            from zprocess import TimeoutError as EventTimeout
            trigger_thread = None
            try:
                if not self._saver.open_for_writing("a"):
                    raise RuntimeError("Could not open camera sidecar for writing")
                self._sink = _FrameSink(self._saver, self.exposures)
                self._ready.set()
                while not self._cancel.is_set():
                    self._saver.get_open_file()  # renew long-held sidecar lock leases
                    try:
                        self._start_event.wait(self._start_token, timeout=0.1)
                        break
                    except EventTimeout:
                        continue
                if self._cancel.is_set():
                    return
                origin = time.monotonic()
                if self.mock:
                    for row in self.exposures:
                        if self._cancel.wait(max(0, origin + row["t"] - time.monotonic())):
                            return
                        self._trigger_times.append(time.monotonic() - origin)
                        self._sink.extend([self.camera.snap()])
                else:
                    trigger_thread = threading.Thread(target=self._send_triggers,
                                                       args=(origin,), daemon=True)
                    trigger_thread.start()
                    self.camera.grab_multiple(len(self.exposures), self._sink)
                file = self._saver.get_open_file()
                file.create_dataset("trigger_sent_times_s", data=self._trigger_times)
                file.create_dataset("requested_start_times_s", data=[row["t"] for row in self.exposures])
                file.create_dataset("frame_headers", dtype=h5py.string_dtype(),
                    data=[json.dumps(header) for header in getattr(self.camera, "last_headers", [])])
                file.attrs["failed_shot"] = len(self._sink) != len(self.exposures)
            except Exception as error:
                self._shot_error = self._shot_error or error
            finally:
                self._ready.set()
                self._cancel.set()
                if trigger_thread is not None:
                    trigger_thread.join()
                try:
                    if not self.mock:
                        self.camera.stop_acquisition()
                except Exception as error:
                    self._shot_error = self._shot_error or error
                finally:
                    self._saver.close_data_file()

        def _restore_manual(self):
            settings, self._manual_settings = self._manual_settings, None
            if settings is not None and (self.mock or self.camera._client.has_control()):
                self.camera.attributes = {}
                self.camera.set_attributes(settings)
            self._start_token = None
            self.h5_filepath = None
            if (self.continuous_dt is not None and self.continuous_thread is None
                    and (self.mock or self.camera._client.has_control())):
                self.start_continuous(self.continuous_dt)

        def transition_to_manual(self):
            import labscript_utils.h5_lock  # noqa: F401
            import h5py
            try:
                started = time.monotonic()
                if self.acquisition_thread is not None:
                    self.acquisition_thread.join(timeout=self.stop_acquisition_timeout)
                    if self.acquisition_thread.is_alive():
                        self.abort()
                        raise RuntimeError("Software camera shot did not finish before timeout")
                    self.acquisition_thread = None
                    if self._shot_error is not None or self._sink is None or len(self._sink) != len(self.exposures):
                        error = self._shot_error or RuntimeError("Incomplete camera shot")
                        self.abort()
                        raise RuntimeError("Software camera shot failed") from error
                    self.logger.info("Camera wait: %.3f s", time.monotonic() - started)
                    started = time.monotonic()
                    if not self._saver.merge_into_shot(cleanup=True):
                        raise RuntimeError("Camera sidecar was not merged")
                    self._saver = None
                    # Keep the established IMAQdx images/<orientation>/<name>/<frametype> API.
                    with h5py.File(self.h5_filepath, "r+") as file:
                        source = file[f"data/{self.device_name}/camera/images"]
                        images = file.require_group("images")
                        orientation = self.orientation or self.device_name
                        if orientation in images:
                            raise RuntimeError(f"Image destination {orientation!r} already exists")
                        target = images.create_group(orientation)
                        target.attrs.update(source.attrs)
                        for name in dict.fromkeys(row["name"] for row in self.exposures):
                            target[name] = source[name]
                    self.logger.info("Camera merge: %.3f s", time.monotonic() - started)
                    self._send_image_to_parent(self._sink.last_image)
                    self._sink = None
                return True
            finally:
                self._restore_manual()

        def abort(self):
            self._cancel.set()
            if self.camera is not None and self._manual_settings is not None:
                self.camera.abort_acquisition()
            if self.acquisition_thread is not None:
                self.acquisition_thread.join()
                self.acquisition_thread = None
            if self._saver is not None:
                self._saver.cleanup()
                self._saver = None
            self._sink = None
            self._restore_manual()
            return True

        def shutdown(self):
            # Shutdown must not restart continuous preview.
            self.continuous_dt = None
            self.abort()
            self.disconnect_from_device()
            self.image_socket.close(linger=0)
            self._metrics.close()

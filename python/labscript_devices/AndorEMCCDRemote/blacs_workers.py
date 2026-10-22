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
from typing import Any, Dict, List, Optional

import numpy as np

from andor_emccd import (AndorEMCCDClient, AndorEMCCDError,
                         NotControllerError)
from andor_emccd.client import DEFAULT_IPC_DIR

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
    use ``arm`` (a kinetic series of n frames, normally external trigger);
    manual continuous mode uses a ``continuous`` run.

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
        """Starts a run: continuous, or a kinetic series of bufferCount."""
        self._abort_acquisition = False
        self._last_seq = None
        self._last_index = -1
        self.last_headers = []
        if continuous:
            self._ensure_control()
            self._client.stop()
            run = self._client.start({"acquisition_mode": "continuous"})
        else:
            # Pass every attribute programmed so far, so the shot does not
            # depend on changes made to the backend from elsewhere (GUI).
            self._ensure_control()
            run = self._client.arm(int(bufferCount), dict(self.attributes))
        for warning in run.get("warnings", []) or []:
            print(f"Warning: {warning}", file=sys.stderr)
        self._run_id = run["run_id"]
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


if IMAQdxCameraWorker is not None:

    class AndorEMCCDRemoteWorker(IMAQdxCameraWorker):
        """IMAQdxCameraWorker talking to the EMCCD backend service.

        Worker init kwargs (from the tab): host, control_port, data_port,
        ipc_dir in addition to the IMAQdxCamera ones.
        """

        interface_class = AndorEMCCDRemoteCamera

        def get_camera(self):
            if self.mock:
                return MockCamera()
            return self.interface_class(
                host=self.host, control_port=self.control_port,
                data_port=self.data_port, ipc_dir=self.ipc_dir)

        def get_attributes_as_dict(self, visibility_level):
            if isinstance(self.camera, AndorEMCCDRemoteCamera):
                return self.camera.get_attributes(visibility_level)
            return super().get_attributes_as_dict(visibility_level)

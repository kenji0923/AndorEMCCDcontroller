"""Parentless labscript camera using BLACS software-trigger scheduling."""

from __future__ import annotations

import labscript_utils.h5_lock  # noqa: F401 (before h5py)
import h5py
import numpy as np
from labscript import compiler, set_passed_properties
from labscript_devices.IMAQdxCamera.labscript_devices import IMAQdxCamera

from .timing import validate_exposures

DEFAULT_CONTROL_PORT = 47100
DEFAULT_DATA_PORT = 47101
DEFAULT_IPC_DIR = "/run/andor-emccd"


class AndorEMCCDRemote(IMAQdxCamera):
    """Remote iXon with software triggers, on a 100 us start-time grid.

    No digital trigger parent/connection is used. ``expose(t, name,
    exposure_time=...)`` schedules an image relative to BLACS's science-start
    notification. All images in a shot share one exposure duration, as required
    by the SDK's run-till-abort software-trigger mode. The grid is a validation
    limit, not a guarantee of timer, transport or camera accuracy.
    """

    description = "Andor iXon EMCCD (software-triggered remote backend)"

    @set_passed_properties(property_names={
        "connection_table_properties": [
            "host", "control_port", "data_port", "ipc_dir", "force_control",
            "poll_interval_ms", "apply_timeout_ms", "edit_expiry_ms",
        ],
        "device_properties": ["minimum_recovery_time"],
    })
    def __init__(self, name, parent_device=None, connection=None,
                 host="localhost", control_port=DEFAULT_CONTROL_PORT,
                 data_port=DEFAULT_DATA_PORT, ipc_dir=DEFAULT_IPC_DIR,
                 serial_number=0, camera_attributes=None,
                 manual_mode_camera_attributes=None, force_control=False,
                 minimum_recovery_time=0.0, poll_interval_ms=100,
                 apply_timeout_ms=3000, edit_expiry_ms=60000, **kwargs):
        if parent_device is not None or connection is not None:
            raise ValueError("Software triggering requires parent_device=None "
                             "and connection=None; external triggering is not implemented")
        if kwargs.get("trigger_duration") is not None:
            raise ValueError("Use exposure_time in expose(), not trigger_duration")
        attrs = dict(camera_attributes or {})
        if attrs.get("trigger_mode", "software") != "software":
            raise ValueError("Buffered shots currently support only trigger_mode='software'")
        attrs.update(trigger_mode="software", acquisition_mode="continuous")
        attrs.setdefault("exposure_time_s", 1e-3)
        manual = dict(manual_mode_camera_attributes or {})
        attrs = {**manual, **attrs}
        for value in (poll_interval_ms, apply_timeout_ms, edit_expiry_ms):
            if value <= 0:
                raise ValueError("Panel timing properties must be positive")
        if minimum_recovery_time < 0 or not np.isfinite(minimum_recovery_time):
            raise ValueError("minimum_recovery_time must be finite and non-negative")
        kwargs.setdefault("pixel_size", [16.0, 16.0])
        kwargs["parentless"] = True
        super().__init__(name, None, None, serial_number,
                         camera_attributes=attrs,
                         manual_mode_camera_attributes=manual,
                         minimum_recovery_time=minimum_recovery_time, **kwargs)
        self.BLACS_connection = f"{host}:{int(control_port)}"

    def expose(self, t, name, frametype="frame", exposure_time=None,
               trigger_duration=None):
        if trigger_duration is not None:
            raise ValueError("Use exposure_time, not trigger_duration")
        if exposure_time is None:
            exposure_time = self.camera_attributes["exposure_time_s"]
        for label in (name, frametype):
            if not isinstance(label, str) or not label or "/" in label or label in (".", ".."):
                raise ValueError("Image name and frametype must be nonempty HDF5 path components")
        row = dict(t=float(t), name=name, frametype=frametype,
                   exposure_time_s=float(exposure_time))
        validate_exposures(self.exposures + [row], self.minimum_recovery_time)
        self.exposures.append(row)
        return float(exposure_time)

    def generate_code(self, hdf5_file):
        stop_time = compiler.master_pseudoclock.stop_time
        rows = validate_exposures(self.exposures, self.minimum_recovery_time, stop_time)
        group = self.init_device_group(hdf5_file)
        group.attrs["stop_time"] = stop_time
        group.attrs["trigger_method"] = "software"
        group.attrs["wait_policy"] = "ignore"
        dtype = [("t", float), ("name", h5py.string_dtype()),
                 ("frametype", h5py.string_dtype()), ("exposure_time_s", float)]
        if rows:
            group.create_dataset("EXPOSURES", data=np.array(
                [tuple(row[key] for key, _ in dtype) for row in rows], dtype=dtype))

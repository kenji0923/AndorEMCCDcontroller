"""BLACS tab for the Andor iXon EMCCD backend service."""

from __future__ import annotations

import labscript_utils.h5_lock  # noqa: F401  (must precede h5py)
import h5py
import labscript_utils.properties
from labscript_devices.IMAQdxCamera.blacs_tabs import IMAQdxCameraTab

_PACKAGE = __name__.rsplit(".", 1)[0]


class AndorEMCCDRemoteTab(IMAQdxCameraTab):
    """IMAQdxCameraTab whose worker also gets the backend address."""

    worker_class = f"{_PACKAGE}.blacs_workers.AndorEMCCDRemoteWorker"

    def initialise_workers(self):
        table = self.settings["connection_table"]
        properties = table.find_by_name(self.device_name).properties
        with h5py.File(table.filepath, "r") as f:
            device_properties = labscript_utils.properties.get(
                f, self.device_name, "device_properties")
        worker_initialisation_kwargs = {
            "serial_number": properties["serial_number"],
            "orientation": properties["orientation"],
            "camera_attributes": device_properties["camera_attributes"],
            "manual_mode_camera_attributes":
                properties["manual_mode_camera_attributes"],
            "mock": properties["mock"],
            "image_receiver_port": self.image_receiver.port,
            "host": properties["host"],
            "control_port": properties["control_port"],
            "data_port": properties["data_port"],
            "ipc_dir": properties["ipc_dir"],
        }
        self.create_worker("main_worker", self.worker_class,
                           worker_initialisation_kwargs)
        self.primary_worker = "main_worker"

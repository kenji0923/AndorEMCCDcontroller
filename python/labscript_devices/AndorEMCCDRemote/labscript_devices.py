"""labscript device for the Andor iXon EMCCD backend service.

The camera is driven by the ``andor_emccd_backend`` service (possibly on
another machine); BLACS talks to it over ZeroMQ via ``andor_emccd``.
Compilation (this module) needs only labscript, not ``andor_emccd``.
"""

from __future__ import annotations

from labscript import set_passed_properties
from labscript_devices.IMAQdxCamera.labscript_devices import IMAQdxCamera

# Kept in sync with andor_emccd.DEFAULT_*_PORT (not imported on purpose).
DEFAULT_CONTROL_PORT = 47100
DEFAULT_DATA_PORT = 47101
DEFAULT_IPC_DIR = "/run/andor-emccd"


class AndorEMCCDRemote(IMAQdxCamera):
    """Andor iXon EMCCD served by andor_emccd_backend, triggered by an edge.

    Each ``expose()`` produces one trigger pulse and one image. At
    ``transition_to_buffered`` the backend is armed for a kinetic series of
    as many frames as there are exposures, so ``camera_attributes`` should
    normally contain ``"trigger_mode": "external"`` (and
    ``manual_mode_camera_attributes`` e.g. ``{"trigger_mode": "software"}``
    for snapping in manual mode).

    Args:
        name: Device name.
        parent_device: Device with the digital output used as trigger.
        connection: Digital output connection on the parent device.
        host: Host name / address of the machine running the backend.
        control_port: TCP port of the backend control (ROUTER) socket.
        data_port: TCP port of the backend data (PUB) socket.
        ipc_dir: Backend IPC directory; used instead of TCP when BLACS runs
            on the backend host and the sockets exist there.
        serial_number: Not used for addressing (the backend owns the
            camera); kept because IMAQdxCamera requires it. May be the
            camera serial for bookkeeping.
        camera_attributes: Backend settings (keys of ``get_settings``).
        manual_mode_camera_attributes: Settings that differ in manual mode.
        **kwargs: Passed to IMAQdxCamera (orientation, pixel_size,
            trigger_duration, stop_acquisition_timeout, ...).
    """

    description = "Andor iXon EMCCD (remote backend)"

    @set_passed_properties(
        property_names={
            "connection_table_properties": [
                "host",
                "control_port",
                "data_port",
                "ipc_dir",
            ],
        }
    )
    def __init__(self, name, parent_device, connection, host="localhost",
                 control_port=DEFAULT_CONTROL_PORT,
                 data_port=DEFAULT_DATA_PORT, ipc_dir=DEFAULT_IPC_DIR,
                 serial_number=0, **kwargs):
        self.host = host
        self.control_port = int(control_port)
        self.data_port = int(data_port)
        self.ipc_dir = ipc_dir
        kwargs.setdefault("pixel_size", [16.0, 16.0])
        IMAQdxCamera.__init__(self, name, parent_device, connection,
                              serial_number, **kwargs)
        # IMAQdxCamera uses the hex serial; the backend address is clearer.
        self.BLACS_connection = f"{host}:{self.control_port}"

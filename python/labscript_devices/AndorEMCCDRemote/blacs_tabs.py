"""FanLab BLACS settings and shot controls for the remote iXon backend."""

from __future__ import annotations

import time

import labscript_utils.h5_lock  # noqa: F401 (must precede h5py)
import h5py
import labscript_utils.properties
from blacs.tab_base_classes import define_state, MODE_MANUAL
from blacs.device_base_class import DeviceTab
from qtutils.qt import QtWidgets
from user_devices.fanlab_devices.utils.front_panel_persistence import FrontPanelPersistence

from .settings_panel import SettingsPanel
from .shot_start import register_shot

_PACKAGE = __name__.rsplit(".", 1)[0]


class AndorEMCCDRemoteTab(DeviceTab):
    worker_class = f"{_PACKAGE}.blacs_workers.AndorEMCCDRemoteWorker"

    def initialise_GUI(self):
        self.supports_smart_programming(False)
        self.supports_remote_value_check(False)
        self._connected = False
        self._pending_settings = {}
        self._draining = False
        self._last_read = None
        self._read_error = ""
        self._action_error = ""
        properties = self.settings["connection_table"].find_by_name(self.device_name).properties
        self._fp = FrontPanelPersistence(self)
        self.panel = SettingsPanel(self._fp, properties)
        self.panel.committed.connect(self.submit_settings)
        self._fp.finalise()
        controls = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(controls)
        self.buttons = {}
        for action, label in (("connect", "Connect"), ("disconnect", "Disconnect"),
                              ("request", "Request control"), ("take_control", "Take control"),
                              ("release", "Release control"),
                              ("read", "Read all")):
            button = QtWidgets.QPushButton(label)
            button.clicked.connect(lambda _, a=action: self.control_action(a))
            row.addWidget(button)
            self.buttons[action] = button
        self.buttons["take_control"].setToolTip(
            "Take settings/acquisition control from the displayed holder. "
            "The previous client loses control.")
        layout = self.get_tab_layout()
        layout.addWidget(controls)
        layout.addWidget(self.panel)
        self._show_panel({"connected": False})
        self.statemachine_timeout_add(properties.get("poll_interval_ms", 100), self.poll_backend)

    def initialise_workers(self):
        table = self.settings["connection_table"]
        properties = table.find_by_name(self.device_name).properties
        with h5py.File(table.filepath, "r") as file:
            device_properties = labscript_utils.properties.get(file, self.device_name, "device_properties")
        kwargs = {key: properties[key] for key in (
            "serial_number", "orientation", "manual_mode_camera_attributes", "mock",
            "host", "control_port", "data_port", "ipc_dir")}
        kwargs.update(camera_attributes=device_properties["camera_attributes"],
                      force_control=properties.get("force_control", False))
        self.create_worker("main_worker", self.worker_class, kwargs)
        self.primary_worker = "main_worker"

    def get_save_data(self):
        data = super().get_save_data()
        data.update(connection_connected=self._connected, camera_panel=self.panel.save())
        data.update(self._fp.save_data())
        return data

    def restore_save_data(self, data):
        # Restoring values/connection must not acquire control or start the camera.
        self.panel.restore(data.get("camera_panel", {}))
        self._fp.restore_save_data(data)
        if data.get("connection_connected", False):
            self.control_action("connect")

    def update_from_settings(self, settings):
        super().update_from_settings(settings)
        self._fp.restore_rows()

    def _show_panel(self, result):
        if not result or "error" in result:
            self._read_error = (result or {}).get("error", "Backend request failed")
            self.panel.set_editable(False)
            age = time.monotonic() - self._last_read if self._last_read else 0
            self.panel.update_banner(f"Backend error ({age:.0f} s since readback): {self._read_error}")
            return
        self._read_error = ""
        self._connected = result["connected"]
        mine = result.get("mine", False)
        control = result.get("control") or {}
        if "action_error" in result:
            self._action_error = result["action_error"]
        self.buttons["connect"].setEnabled(not self._connected)
        for action in ("disconnect", "request", "read"):
            self.buttons[action].setEnabled(self._connected)
        self.buttons["request"].setEnabled(self._connected and not mine)
        self.buttons["take_control"].setEnabled(self._connected and bool(control) and not mine)
        self.buttons["release"].setEnabled(mine)
        self.panel.set_editable(mine)
        if not self._connected:
            self.panel.update_banner("Disconnected")
            return
        self._last_read = time.monotonic()
        self.panel.readback(result["settings"], result["info"])
        holder = control.get("holder", "unclaimed")
        status = result.get("status", {})
        access = "Connected (control held)" if mine else "Connected (read-only)"
        text = f"{access} · {time.strftime('%H:%M:%S')} · Control: {holder}"
        if not mine:
            text += " · Use Take control" if control else " · Use Request control"
        if "temperature_c" in status:
            text += f" · {status['temperature_c']:.1f} °C · {status.get('state', '')}"
        if self._action_error:
            text += f" · {self._action_error}"
        self.panel.update_banner(text)

    @define_state(MODE_MANUAL, queue_state_indefinitely=True)
    def control_action(self, action):
        self._action_error = ""
        result = yield self.queue_work(self.primary_worker, "panel_action", action)
        self._show_panel(result)

    @define_state(MODE_MANUAL, queue_state_indefinitely=False, delete_stale_states=True)
    def poll_backend(self):
        # BLACS requires a generator state to yield at least once, including
        # startup while disconnected. The worker's disconnected read is local.
        result = yield self.queue_work(self.primary_worker, "panel_action", "read")
        self._show_panel(result)

    def submit_settings(self, patch):
        self._pending_settings.update(patch)
        if not self._draining:
            self._draining = True
            self.drain_settings()

    @define_state(MODE_MANUAL, queue_state_indefinitely=False)
    def drain_settings(self):
        try:
            while True:
                patch, self._pending_settings = self._pending_settings, {}
                action = "settings" if patch else "read"
                result = yield self.queue_work(self.primary_worker, "panel_action", action, patch)
                if patch:
                    self.panel.applied(patch, result)
                self._show_panel(result)
                if not self._pending_settings:
                    break
        finally:
            self._draining = False

    def transition_to_buffered(self, h5_file, notify_queue):
        self.prepare_software_shot(h5_file)
        super().transition_to_buffered(h5_file, notify_queue)

    @define_state(MODE_MANUAL, queue_state_indefinitely=True)
    def prepare_software_shot(self, h5_file):
        self._pending_settings.clear()
        self._draining = False
        token = register_shot(self.device_name, h5_file)
        yield self.queue_work(self.primary_worker, "prepare_shot_start", token)

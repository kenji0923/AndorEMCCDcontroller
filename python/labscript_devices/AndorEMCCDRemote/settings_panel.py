"""Backend settings editor using FanLab commit, reconciliation and persistence."""

import time

from qtutils.qt import QtCore, QtWidgets
from user_devices.fanlab_devices.utils.value_widgets import (
    CommittingDoubleSpinBox, unit_conversion_for, SYNCED, EDITING, PENDING,
    CONFIRMING, REJECTED,
)

# Same settings and groupings as the native SettingsPanel. Numeric tuples are
# (minimum, maximum, decimals, base unit); enum values remain backend values.
GROUPS = {
    "Acquisition": {
        "acquisition_mode": ["single", "accumulate", "kinetic", "continuous"],
        "trigger_mode": ["internal", "external", "external_start", "external_exposure", "software"],
        "exposure_time_s": (0, 3600, 6, "s"),
        "number_kinetics": (1, 100000000, 0, ""),
        "kinetic_cycle_time_s": (0, 3600, 6, "s"),
        "number_accumulations": (1, 1000000, 0, ""),
        "accumulation_cycle_time_s": (0, 3600, 6, "s"),
        "frame_transfer": bool, "fast_external_trigger": bool,
        "trigger_invert": bool, "metadata": bool,
    },
    "Readout": {
        "output_amplifier": [0, 1], "hs_speed_index": [0],
        "vs_speed_index": [-1, 0], "vs_amplitude": [0],
        "preamp_gain_index": [0], "baseline_clamp": bool,
        "count_convert_mode": [0, 1, 2],
        "count_convert_wavelength_nm": (100, 2000, 1, "nm"),
    },
    "EM gain": {
        "em_gain_enabled": bool, "em_gain": (0, 4095, 0, ""),
        "em_gain_mode": [0, 1, 2, 3], "em_advanced": bool,
    },
    "Image area": {
        "hbin": (1, 4096, 0, ""), "vbin": (1, 4096, 0, ""),
        "hstart": (1, 65535, 0, ""), "hend": (0, 65535, 0, ""),
        "vstart": (1, 65535, 0, ""), "vend": (0, 65535, 0, ""),
        "isolated_crop": bool, "crop_width": (0, 65535, 0, ""),
        "crop_height": (0, 65535, 0, ""),
    },
    "Cooling": {
        # BLACS stores current_units in a fixed ASCII HDF5 field.
        "cooler_on": bool, "target_temperature_c": (-150, 50, 0, "degC"),
        "fan_mode": ["full", "low", "off"], "cooler_persist_on_shutdown": bool,
    },
    "External shutter": {
        "shutter_mode": ["auto", "open", "closed"], "shutter_ttl_high_open": bool,
        "shutter_opening_time_ms": (0, 10000, 0, "ms"),
        "shutter_closing_time_ms": (0, 10000, 0, "ms"),
    },
}


class SettingsPanel(QtWidgets.QWidget):
    committed = QtCore.Signal(dict)

    def __init__(self, persistence, properties):
        super().__init__()
        self._fp = persistence
        self.fields = {}
        self.specs = {}
        self.last = {}
        self.confirming = {}
        self.errors = {}
        self.rejected = set()
        self._editable = False
        self._supported = {}
        self.last_activity = time.monotonic()
        self.apply_timeout = properties.get("apply_timeout_ms", 3000) / 1000
        self.edit_expiry = properties.get("edit_expiry_ms", 60000) / 1000
        layout = QtWidgets.QVBoxLayout(self)
        self.banner = QtWidgets.QLabel("Disconnected")
        self.banner.setWordWrap(True)
        layout.addWidget(self.banner)
        self.error_label = QtWidgets.QLabel()
        self.error_label.setWordWrap(True)
        self.error_label.setTextFormat(QtCore.Qt.PlainText)
        self.error_label.setStyleSheet("color: #b02020")
        self.error_label.hide()
        layout.addWidget(self.error_label)
        self.tabs = QtWidgets.QTabWidget()
        layout.addWidget(self.tabs)
        for title, specs in GROUPS.items():
            page = QtWidgets.QWidget()
            form = QtWidgets.QFormLayout(page)
            scroll = QtWidgets.QScrollArea()
            scroll.setWidgetResizable(True)
            scroll.setWidget(page)
            self.tabs.addTab(scroll, title)
            for key, spec in specs.items():
                self.specs[key] = spec
                if isinstance(spec, tuple):
                    widget = CommittingDoubleSpinBox()
                    low, high, decimals, unit = spec
                    widget.setDecimals(decimals)
                    widget.set_units(unit, unit_conversion_for(unit))
                    widget.set_hard_limits(low, high)
                    widget.set_base_step(1e-4 if unit == "s" else 1)
                    if unit == "s":
                        widget.set_display_unit("ms")
                    widget.set_commit_on_editing_finished(False)
                    widget.userInput.connect(self.activity)
                    widget.valueCommitted.connect(lambda _, k=key: self.commit(k))
                    self._fp.bind_committing_spinbox(f"settings/{key}", widget)
                elif spec is bool:
                    widget = QtWidgets.QCheckBox()
                    self._fp.bind_checkbox(f"settings/{key}", widget)
                    widget.clicked.connect(lambda _, k=key: self.commit(k))
                else:
                    widget = QtWidgets.QComboBox()
                    for value in spec:
                        widget.addItem(str(value).replace("_", " "), value)
                    widget.activated.connect(lambda _, k=key: self.commit(k))
                self.fields[key] = widget
                if isinstance(spec, list):
                    bind = self._fp.bind_string if isinstance(spec[0], str) else self._fp.bind_value
                    bind(f"settings/{key}", lambda k=key: self.value(k),
                         lambda value, k=key: self.set_value(k, value))
                widget.installEventFilter(self)
                if isinstance(widget, CommittingDoubleSpinBox):
                    widget.lineEdit().installEventFilter(self)
                label = key.replace("_", " ").removesuffix(" s").removesuffix(" c").capitalize()
                form.addRow(label, widget)
        self.set_editable(False)

    def activity(self, *_):
        self.last_activity = time.monotonic()

    def eventFilter(self, watched, event):
        if event.type() in (QtCore.QEvent.KeyPress, QtCore.QEvent.FocusIn):
            self.activity()
        if event.type() == QtCore.QEvent.KeyPress:
            key = next((key for key, w in self.fields.items()
                        if watched is w or w.isAncestorOf(watched)), None)
            if event.key() == QtCore.Qt.Key_Escape:
                keys = [key] if key and self.state(key) in (PENDING, REJECTED, EDITING) else self.fields
                for field in keys:
                    if field in self.last:
                        self.set_value(field, self.last[field])
                    self.errors.pop(field, None)
                    self.rejected.discard(field)
                    self.confirming.pop(field, None)
                self._fp.push_all()
                return True
            if event.key() in (QtCore.Qt.Key_Return, QtCore.Qt.Key_Enter):
                if key and isinstance(self.fields[key], CommittingDoubleSpinBox):
                    self.fields[key].interpretText()
                    self.commit(key, retry=True)
                    return True
        return super().eventFilter(watched, event)

    def value(self, key):
        w = self.fields[key]
        if isinstance(w, CommittingDoubleSpinBox):
            value = w.base_value()
            return int(round(value)) if self.specs[key][2] == 0 else value
        if isinstance(w, QtWidgets.QCheckBox):
            return w.isChecked()
        return w.currentData()

    def set_value(self, key, value):
        w = self.fields[key]
        if isinstance(w, CommittingDoubleSpinBox):
            w.set_base_value_silently(value)
        else:
            old = w.blockSignals(True)
            if isinstance(w, QtWidgets.QCheckBox):
                w.setChecked(bool(value))
            else:
                index = w.findData(value)
                if index < 0:
                    w.addItem(str(value), value)
                    index = w.count() - 1
                w.setCurrentIndex(index)
            w.blockSignals(old)

    def matches(self, key, value):
        current = self.value(key)
        if isinstance(self.fields[key], CommittingDoubleSpinBox):
            return abs(current - value) <= max(1e-12, self.fields[key].display_quantum_base() / 2)
        return current == value

    def focused(self, key):
        widget = self.fields[key]
        focus = QtWidgets.QApplication.focusWidget()
        return focus is widget or (focus is not None and widget.isAncestorOf(focus))

    def state(self, key):
        if key in self.confirming:
            return CONFIRMING if self.matches(key, self.confirming[key][0]) else PENDING
        if key in self.rejected:
            return REJECTED
        if key in self.last and not self.matches(key, self.last[key]):
            return PENDING
        return EDITING if self.focused(key) else SYNCED

    def commit(self, key, retry=False):
        self.activity()
        patch = {field: self.value(field) for field in self.fields
                 if self.state(field) == PENDING
                 or (self.state(field) == REJECTED and (retry or field == key))}
        # A fresh checkbox/combobox edit is already visible when clicked fires.
        for field, value in patch.items():
            self.errors.pop(field, None)
            self.rejected.discard(field)
            self.confirming[field] = (value, time.monotonic())
            w = self.fields[field]
            if isinstance(w, CommittingDoubleSpinBox):
                w.reset_commit_dedup()
        self._fp.push_all()
        if patch:
            self.committed.emit(patch)

    def applied(self, patch, result):
        if "error" in result:
            for key, value in patch.items():
                if self.confirming.get(key, (None,))[0] == value:
                    self.confirming.pop(key, None)
                    self.errors[key] = result["error"]
                    self.rejected.add(key)

    def choices(self, key, pairs):
        w = self.fields[key]
        if not pairs or self.state(key) in (PENDING, CONFIRMING, REJECTED) or self.focused(key):
            return
        pairs = list(pairs)
        if [(w.itemText(i), w.itemData(i)) for i in range(w.count())] == pairs:
            return
        value = self.value(key)
        old = w.blockSignals(True)
        w.clear()
        for label, item in pairs:
            w.addItem(label, item)
        w.blockSignals(old)
        self.set_value(key, value)

    def readback(self, settings, info):
        for key, dimension in (("hbin", "sensor_width"), ("vbin", "sensor_height"),
                               ("hstart", "sensor_width"), ("hend", "sensor_width"),
                               ("vstart", "sensor_height"), ("vend", "sensor_height"),
                               ("crop_width", "sensor_width"), ("crop_height", "sensor_height")):
            if dimension in info:
                self.fields[key].set_hard_limits(self.specs[key][0], info[dimension])
        if "temperature_range_c" in info:
            self.fields["target_temperature_c"].set_hard_limits(*info["temperature_range_c"])
        if "em_gain_max" in info:
            self.fields["em_gain"].set_hard_limits(0, info["em_gain_max"])
        for key, capability in (("isolated_crop", "has_isolated_crop"),
                                ("frame_transfer", "has_frame_transfer"),
                                ("count_convert_mode", "has_count_convert")):
            self._supported[key] = info.get(capability, True)
            self.fields[key].setEnabled(self._editable and self._supported[key])
        for key in ("trigger_mode", "acquisition_mode"):
            self.choices(key, [(s.replace("_", " "), s) for s in info.get(key + "s", [])])
        amps = info.get("amplifiers", [])
        self.choices("output_amplifier", [(a["name"], i) for i, a in enumerate(amps)])
        amp = int(settings.get("output_amplifier", 0))
        if 0 <= amp < len(amps):
            self.choices("hs_speed_index", [(f"{v} MHz", i) for i, v in enumerate(amps[amp]["hs_speeds_mhz"])])
        self.choices("vs_speed_index", [("Fastest recommended", -1)] +
                     [(f"{v} µs", i) for i, v in enumerate(info.get("vs_speeds_us", []))])
        self.choices("vs_amplitude", [(str(v), i) for i, v in enumerate(info.get("vs_amplitudes", []))])
        self.choices("preamp_gain_index", [(f"{v}x", i) for i, v in enumerate(info.get("preamp_gains", []))])
        self.choices("em_gain_mode", [("DAC 0–255", 0), ("DAC 0–4095", 1),
                                      ("Linear", 2), ("Real EM gain", 3)])
        self.choices("count_convert_mode", [("Counts", 0), ("Electrons", 1), ("Photons", 2)])
        now = time.monotonic()
        for key, value in settings.items():
            if key not in self.fields:
                continue
            state = self.state(key)
            self.last[key] = value
            if key in self.confirming:
                requested, sent = self.confirming[key]
                if not self.matches(key, requested):
                    self.confirming.pop(key)  # typing supersedes an outstanding commit
                elif self.matches(key, value) or now - sent >= self.apply_timeout:
                    self.confirming.pop(key)
                    self.set_value(key, value)
            elif self.matches(key, value):
                self.rejected.discard(key)
            elif not self.focused(key) and (state in (SYNCED, EDITING) or now - self.last_activity >= self.edit_expiry):
                self.set_value(key, value)
                self.rejected.discard(key)
        self._fp.push_all()

    def update_banner(self, text):
        counts = {PENDING: 0, CONFIRMING: 0, REJECTED: 0}
        for key, widget in self.fields.items():
            state = self.state(key)
            if state in counts:
                counts[state] += 1
            if isinstance(widget, CommittingDoubleSpinBox):
                widget.set_sync_state(state)
            else:
                color = {PENDING: "#fff0bc", CONFIRMING: "#c9e1ff", REJECTED: "#ffcaca"}.get(state)
                widget.setStyleSheet(f"background: {color}" if color else "")
        detail = " · ".join(f"{n} {s}" for s, n in counts.items() if n)
        if counts[PENDING] or counts[REJECTED]:
            detail += f" · Enter commits / Esc reverts · expiry {max(0, int(self.edit_expiry - time.monotonic() + self.last_activity))} s"
        self.banner.setText(text + (" · " + detail if detail else ""))
        self.error_label.setText("\n".join(f"{key}: {error}" for key, error in self.errors.items()))
        self.error_label.setVisible(bool(self.errors))

    def set_editable(self, enabled):
        self._editable = enabled
        for key, widget in self.fields.items():
            widget.setEnabled(enabled and self._supported.get(key, True))

    def save(self):
        return {key: w.save_state() if isinstance(w, CommittingDoubleSpinBox)
                else self.value(key) for key, w in self.fields.items()}

    def restore(self, data):
        for key, value in data.items():
            if key not in self.fields:
                continue
            w = self.fields[key]
            if isinstance(w, CommittingDoubleSpinBox) and isinstance(value, dict):
                w.restore_state(value)
            else:
                self.set_value(key, value)

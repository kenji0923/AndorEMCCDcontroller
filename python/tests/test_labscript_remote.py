"""Software-shot and FanLab panel regression tests (DAQ environment).

Run with READTHEDOCS=1 to use local HDF5 files without a zlock service.
Integration cases use only the simulated backend selected by AEC_TEST_*.
"""

import logging
import os
from pathlib import Path
import sys
import threading
import time
from types import SimpleNamespace
from unittest.mock import MagicMock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest

# Import the installed package before exposing this repository's source folder.
pytest.importorskip("labscript_devices")
import labscript_utils.h5_lock  # noqa: F401
import h5py

_PYTHON = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_PYTHON))
sys.path.insert(0, str(_PYTHON / "labscript_devices"))

from AndorEMCCDRemote.timing import validate_start_time
from AndorEMCCDRemote.labscript_devices import AndorEMCCDRemote
from AndorEMCCDRemote.blacs_workers import AndorEMCCDRemoteWorker
from AndorEMCCDRemote.shot_start import ShotStartBridge
from andor_emccd import AndorEMCCDClient, ControlDeniedError
from labscript import labscript_init, labscript_cleanup, start, stop, compiler
from labscript_devices.DummyPseudoclock.labscript_devices import DummyPseudoclock


@pytest.mark.parametrize("value", [0, .0001, .001, .1 + .2, 1.2345, 1234.5678])
def test_valid_start_grid(value):
    assert validate_start_time(value) == value


@pytest.mark.parametrize("value", [1e-6, 5e-5, .10001, .10005, 1000.00001,
                                  -0.1, float("nan"), float("inf")])
def test_invalid_start_grid(value):
    with pytest.raises(ValueError):
        validate_start_time(value)


def compile_shot(path, times=(.1, .2), exposure=.001):
    labscript_init(str(path), labscript_file=__file__, new=True, overwrite=True)
    try:
        DummyPseudoclock("clock")
        camera = AndorEMCCDRemote("emccd", orientation="side",
            camera_attributes={"hbin": 4, "vbin": 4, "exposure_time_s": exposure},
            stop_acquisition_timeout=1.0)
        start()
        for i, t in enumerate(times):
            camera.expose(t, "fluorescence", frametype=f"frame{i}")
        stop(max(times, default=0) + .2)
    finally:
        labscript_cleanup()
    return path


def test_compile_software_shot(tmp_path):
    path = compile_shot(tmp_path / "shot.h5")
    with h5py.File(path) as file:
        group = file["devices/emccd"]
        assert group.attrs["trigger_method"] == "software"
        assert list(group["EXPOSURES"]["t"]) == [.1, .2]
        assert list(group["EXPOSURES"]["exposure_time_s"]) == [.001, .001]
        assert "emccd_trigger" not in file["devices"]


def test_compile_rejects_fine_start(tmp_path):
    with pytest.raises(ValueError, match="0.1 ms"):
        compile_shot(tmp_path / "bad.h5", (.10001,))


def test_compile_rejects_duration_changes_and_waits(tmp_path):
    labscript_init(str(tmp_path / "bad.h5"), labscript_file=__file__, new=True)
    try:
        DummyPseudoclock("clock")
        camera = AndorEMCCDRemote("emccd")
        start()
        camera.expose(.1, "atoms", exposure_time=.001)
        with pytest.raises(ValueError, match="same exposure_time"):
            camera.expose(.2, "background", exposure_time=.002)
        compiler.wait_table[.15] = ("wait", 1)
        with h5py.File(tmp_path / "validation.h5", "w") as file:
            with pytest.raises(ValueError, match="waits"):
                camera.generate_code(file)
    finally:
        labscript_cleanup()


class StartEvent:
    def __init__(self, *args, **kwargs):
        self.events = {}

    def post(self, token):
        self.events.setdefault(token, threading.Event()).set()

    def wait(self, token, timeout):
        from zprocess import TimeoutError
        if not self.events.setdefault(token, threading.Event()).wait(timeout):
            raise TimeoutError()


def test_start_bridge_uses_fresh_tokens_and_matches_shot(monkeypatch):
    monkeypatch.setattr("labscript_utils.ls_zprocess.Event", StartEvent)
    bridge = ShotStartBridge()
    old = bridge.register("emccd", "/tmp/shot.h5")
    new = bridge.register("emccd", "/tmp/shot.h5")
    assert old != new
    bridge.science_starting("/tmp/other.h5")
    assert not bridge.event.events
    bridge.science_starting("/tmp/shot.h5")
    assert list(bridge.event.events) == [new]
    assert not bridge.pending


@pytest.fixture
def worker(monkeypatch):
    kwargs = dict(host=os.environ.get("AEC_TEST_HOST", "127.0.0.1"),
        control_port=int(os.environ.get("AEC_TEST_CONTROL_PORT", "47550")),
        data_port=int(os.environ.get("AEC_TEST_DATA_PORT", "47551")),
        ipc_dir=os.environ.get("AEC_TEST_IPC_DIR", "/tmp/aec-blacs-test"))
    client = AndorEMCCDClient(timeout_s=.5, **kwargs)
    try:
        if not client.ping().get("simulated"):
            pytest.skip("Only simulated cameras may run these tests")
    except Exception:
        pytest.skip("Isolated simulated backend is not reachable")
    finally:
        client.close()
    monkeypatch.setattr("labscript_utils.ls_zprocess.Event", StartEvent)
    monkeypatch.setattr("labscript_utils.ls_zprocess.Context", MagicMock())
    monkeypatch.setattr("pythonlib.online_monitor.status.StatusMetrics", MagicMock())
    worker = object.__new__(AndorEMCCDRemoteWorker)
    worker.__dict__.update(kwargs, device_name="emccd", mock=False,
        force_control=False, camera_attributes={}, manual_mode_camera_attributes={},
        orientation="side", image_receiver_port=1, parent_host="localhost",
        logger=logging.getLogger("camera-test"))
    worker.init()
    worker._send_image_to_parent = MagicMock()
    yield worker
    worker.shutdown()


def test_worker_startup_does_not_connect(worker):
    assert worker.camera is None


def test_shot_waits_for_start_saves_and_restores(worker, tmp_path):
    path = compile_shot(tmp_path / "shot.h5")
    worker.connect_to_device()
    original = worker.camera._client.get_settings()
    worker.prepare_shot_start("attempt")
    worker.transition_to_buffered("emccd", str(path), {}, False)
    time.sleep(.15)  # longer than the first scheduled time, still no notification
    assert worker.camera._client.get_status()["state"] == "acquiring"
    assert worker.camera._acq_client.fetch(worker.camera._run_id)[1] == []
    worker._start_event.post("attempt")
    assert worker.transition_to_manual()
    assert worker.camera._client.get_settings() == original
    with h5py.File(path) as file:
        assert file["images/side/fluorescence/frame0"].shape == (128, 128)
        assert file["images/side/fluorescence/frame1"].shape == (128, 128)
        sent = file["data/emccd/camera/images/trigger_sent_times_s"][:]
        assert len(sent) == 2 and all(sent >= [.1, .2])
        assert not file["images/side"].attrs["failed_shot"]
    assert not Path(str(path) + ".tmp").exists()


def test_runtime_rejects_old_or_modified_shot_before_start(worker, tmp_path):
    path = compile_shot(tmp_path / "shot.h5")
    with h5py.File(path, "r+") as file:
        dataset = file["devices/emccd/EXPOSURES"]
        rows = dataset[:]
        rows[0]["t"] = .10001
        dataset[:] = rows
    worker.prepare_shot_start("bad")
    with pytest.raises(ValueError, match="0.1 ms"):
        worker.transition_to_buffered("emccd", str(path), {}, False)
    assert worker.camera._run_id is None
    assert not Path(str(path) + ".tmp").exists()


def test_every_buffered_transition_requests_control_even_empty(worker, tmp_path, monkeypatch):
    path = compile_shot(tmp_path / "empty.h5", ())
    worker.connect_to_device()
    request = MagicMock(wraps=worker.camera._client.acquire_control)
    monkeypatch.setattr(worker.camera._client, "acquire_control", request)
    for _ in range(2):
        worker.transition_to_buffered("emccd", str(path), {}, False)
        worker.transition_to_manual()
    assert request.call_count == 2


def test_buffered_control_denial_is_a_failure(worker, tmp_path):
    path = compile_shot(tmp_path / "shot.h5")
    with AndorEMCCDClient(host=worker.host, control_port=worker.control_port,
                         ipc_dir=worker.ipc_dir, name="GUI-test") as gui:
        gui.acquire_control()
        with pytest.raises(ControlDeniedError):
            worker.transition_to_buffered("emccd", str(path), {}, False)
        assert gui.has_control()


def test_explicit_control_takeover_with_identical_display_names(worker):
    worker.connect_to_device()
    worker.camera._client.name = "micro0"
    with AndorEMCCDClient(host=worker.host, control_port=worker.control_port,
                         ipc_dir=worker.ipc_dir, name="micro0") as gui:
        gui.acquire_control()
        refused = worker.panel_action("request")
        assert refused["connected"] and not refused["mine"]
        assert "action_error" in refused
        assert gui.has_control()
        accepted = worker.panel_action("take_control")
        assert accepted["mine"]
        assert not gui.has_control()
        assert accepted["control"]["holder_id"] == worker.camera._client.client_id
        worker.panel_action("release")


@pytest.mark.parametrize("started", [False, True])
def test_abort_cancels_pending_triggers_and_cleans_sidecar(worker, tmp_path, started):
    path = compile_shot(tmp_path / "shot.h5", (.5, .7))
    worker.prepare_shot_start("abort")
    worker.transition_to_buffered("emccd", str(path), {}, False)
    if started:
        worker._start_event.post("abort")
    before = time.monotonic()
    assert worker.abort_buffered()
    assert time.monotonic() - before < .5
    assert worker.acquisition_thread is None
    assert not Path(str(path) + ".tmp").exists()
    with h5py.File(path) as file:
        assert "images" not in file or "side" not in file["images"]


def test_trigger_error_is_reported_and_not_saved(worker, tmp_path, monkeypatch):
    path = compile_shot(tmp_path / "shot.h5")
    worker.prepare_shot_start("error")
    worker.transition_to_buffered("emccd", str(path), {}, False)
    monkeypatch.setattr(worker.camera._client, "software_trigger",
                        MagicMock(side_effect=RuntimeError("trigger refused")))
    worker._start_event.post("error")
    with pytest.raises(RuntimeError, match="shot failed") as caught:
        worker.transition_to_manual()
    assert "trigger refused" in str(caught.value.__cause__)
    assert not Path(str(path) + ".tmp").exists()


@pytest.fixture
def panel():
    from qtutils.qt.QtWidgets import QApplication
    from user_devices.fanlab_devices.utils.front_panel_persistence import FrontPanelPersistence
    from AndorEMCCDRemote.settings_panel import SettingsPanel
    app = QApplication.instance() or QApplication([])
    tab = SimpleNamespace(settings={"front_panel_settings": {}})
    fp = FrontPanelPersistence(tab)
    panel = SettingsPanel(fp, {})
    fp.finalise()
    panel.set_editable(True)
    panel.readback({"exposure_time_s": .001, "em_gain": 10, "cooler_on": False}, {})
    yield panel, fp, tab, app
    panel.close()


def test_panel_bulk_commit_and_silent_restore(panel):
    panel, fp, tab, app = panel
    committed = []
    panel.committed.connect(committed.append)
    panel.fields["em_gain"].set_base_value_silently(20)
    panel.fields["exposure_time_s"].set_base_value_silently(.002)
    panel.commit("em_gain")
    assert committed == [{"exposure_time_s": .002, "em_gain": 20}]
    assert tab.settings["front_panel_settings"]["~settings/em_gain"]["base_value"] == 20
    saved = panel.save()
    panel.restore(saved)
    fp.restore_save_data(fp.save_data())
    assert len(committed) == 1
    panel.readback({"exposure_time_s": .002, "em_gain": 20}, {})
    assert not panel.confirming


def test_panel_does_not_overwrite_pending_edits_and_retries(panel):
    panel, fp, tab, app = panel
    panel.fields["em_gain"].set_base_value_silently(20)
    panel.readback({"em_gain": 10}, {})
    assert panel.value("em_gain") == 20
    panel.commit("em_gain")
    panel.applied({"em_gain": 20}, {"error": "not controller"})
    committed = []
    panel.committed.connect(committed.append)
    panel.commit("em_gain", retry=True)
    assert committed == [{"em_gain": 20}]


def test_tab_builds_with_standard_inputs_and_persistence(monkeypatch, tmp_path):
    from qtutils.qt import QtWidgets
    from blacs.device_base_class import DeviceTab
    from AndorEMCCDRemote.blacs_tabs import AndorEMCCDRemoteTab
    monkeypatch.setattr(DeviceTab, "__init__", lambda self: None)
    monkeypatch.setattr("labscript_devices.IMAQdxCamera.blacs_tabs.ImageReceiver", MagicMock())
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    tab = AndorEMCCDRemoteTab()
    tab._device_name = "emccd"
    tab.settings = {"connection_table": SimpleNamespace(
        find_by_name=lambda _: SimpleNamespace(properties={})), "front_panel_settings": {}}
    widget = QtWidgets.QWidget()
    layout = QtWidgets.QVBoxLayout(widget)
    tab.get_tab_layout = lambda: layout
    tab.supports_smart_programming = MagicMock()
    tab.statemachine_timeout_add = MagicMock()
    tab.event_queue = MagicMock()
    tab.initialise_GUI()
    tab.primary_worker = "main_worker"
    # BLACS calls next() once outside its StopIteration handler. A timer state
    # must therefore yield even before Connect has been pressed.
    tab.poll_backend()
    function, (args, kwargs) = tab.event_queue.put.call_args.args[3]
    state = function(tab, *args, **kwargs)
    assert next(state) == ("main_worker", "panel_action", ("read",), {})
    with pytest.raises(StopIteration):
        state.send({"connected": False})
    assert not tab._connected
    # Clearing a queued settings drain during a mode change must be safe too.
    tab.drain_settings()
    function, (args, kwargs) = tab.event_queue.put.call_args.args[3]
    state = function(tab, *args, **kwargs)
    assert next(state) == ("main_worker", "panel_action", ("read", {}), {})
    with pytest.raises(StopIteration):
        state.send({"connected": False})
    assert not tab.buttons["request"].isEnabled()
    tab.restore_save_data({"camera_panel": {}, "connection_connected": True})
    assert tab.event_queue.put.called
    widget.resize(1000, 900)
    widget.show()
    app.processEvents()
    # Read-only connection should enable Request control but keep settings locked.
    tab._show_panel(dict(connected=True, mine=False, control={"holder": "GUI"},
                         settings={"em_gain": 50}, info={}, status={}))
    assert tab.buttons["request"].isEnabled()
    assert tab.buttons["take_control"].isEnabled()
    assert "read-only" in tab.panel.banner.text()
    assert not tab.panel.fields["em_gain"].isEnabled()
    assert tab.panel.value("em_gain") == 50
    tab._show_panel(dict(connected=True, mine=True, control={"holder": "BLACS:emccd"},
                         settings={"em_gain": 50}, info={}, status={}))
    assert tab.panel.fields["em_gain"].isEnabled()
    assert tab.ui.pushButton_snap.isEnabled()
    assert not tab.buttons["take_control"].isEnabled()
    widget.grab().save(str(tmp_path / "blacs-tab.png"))
    widget.close()


def test_disconnected_panel_read_does_not_connect_or_error():
    worker = object.__new__(AndorEMCCDRemoteWorker)
    worker.camera = None
    assert worker.panel_action("read") == {"connected": False}
    assert worker.camera is None


def test_front_panel_units_can_be_saved_in_blacs_ascii_fields(panel):
    editor, fp, tab, app = panel
    fp.push_all()
    # BLACS's front_panel HDF5 dataset uses fixed-length byte strings.
    import numpy as np
    units = [row["current_units"] for row in tab.settings["front_panel_settings"].values()]
    encoded = np.asarray(units, dtype="S64")
    assert b"degC" in encoded


def test_panel_new_commit_supersedes_inflight_value(panel):
    panel, fp, tab, app = panel
    committed = []
    panel.committed.connect(committed.append)
    panel.fields["em_gain"].set_base_value_silently(20)
    panel.commit("em_gain")
    panel.fields["em_gain"].set_base_value_silently(30)
    panel.commit("em_gain")
    assert committed == [{"em_gain": 20}, {"em_gain": 30}]
    panel.readback({"em_gain": 20}, {})
    assert panel.value("em_gain") == 30
    panel.readback({"em_gain": 30}, {})
    assert not panel.confirming

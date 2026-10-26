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
import numpy as np
from PIL import Image

# Import the installed package before exposing this repository's source folder.
pytest.importorskip("labscript_devices")
import labscript_utils.h5_lock  # noqa: F401
import h5py

_PYTHON = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_PYTHON))
sys.path.insert(0, str(_PYTHON / "labscript_devices"))

from AndorEMCCDRemote.timing import validate_start_time
from AndorEMCCDRemote.labscript_devices import AndorEMCCDRemote
from AndorEMCCDRemote.blacs_workers import AndorEMCCDRemoteWorker, _save_shot_previews
from AndorEMCCDRemote.shot_start import ShotStartBridge
from andor_emccd import AndorEMCCDClient, ControlDeniedError
from labscript import labscript_init, labscript_cleanup, start, stop, wait
from labscript_devices.DummyPseudoclock.labscript_devices import DummyPseudoclock


@pytest.mark.parametrize("value", [0, .0001, .001, .1 + .2, 1.2345, 1234.5678])
def test_valid_start_grid(value):
    assert validate_start_time(value) == value


@pytest.mark.parametrize("value", [1e-6, 5e-5, .10001, .10005, 1000.00001,
                                  -0.1, float("nan"), float("inf")])
def test_invalid_start_grid(value):
    with pytest.raises(ValueError):
        validate_start_time(value)


def compile_shot(path, times=(.1, .2), exposure=.001, with_wait=False):
    labscript_init(str(path), labscript_file=__file__, new=True, overwrite=True)
    try:
        DummyPseudoclock("clock")
        camera = AndorEMCCDRemote("emccd", orientation="side",
            camera_attributes={"hbin": 4, "vbin": 4, "exposure_time_s": exposure},
            stop_acquisition_timeout=1.0)
        start()
        if with_wait:
            wait("hardware_pause", .15, timeout=60)
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


def test_compile_rejects_duration_changes(tmp_path):
    labscript_init(str(tmp_path / "bad.h5"), labscript_file=__file__, new=True)
    try:
        DummyPseudoclock("clock")
        camera = AndorEMCCDRemote("emccd")
        start()
        camera.expose(.1, "atoms", exposure_time=.001)
        with pytest.raises(ValueError, match="same exposure_time"):
            camera.expose(.2, "background", exposure_time=.002)
    finally:
        labscript_cleanup()


def test_compile_ignores_wait_without_changing_exposure_times(tmp_path):
    path = compile_shot(tmp_path / "wait.h5", with_wait=True)
    with h5py.File(path) as file:
        assert len(file["waits"]) == 1
        group = file["devices/emccd"]
        assert group.attrs["wait_policy"] == "ignore"
        assert list(group["EXPOSURES"]["t"]) == [.1, .2]


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
    monkeypatch.setattr("pythonlib.online_monitor.status.StatusMetrics", MagicMock())
    worker = object.__new__(AndorEMCCDRemoteWorker)
    worker.__dict__.update(kwargs, device_name="emccd", mock=False,
        force_control=False, camera_attributes={}, manual_mode_camera_attributes={},
        orientation="side",
        logger=logging.getLogger("camera-test"))
    worker.init()
    yield worker
    worker.shutdown()


def test_worker_startup_does_not_connect(worker):
    assert worker.camera is None
    assert not hasattr(worker, "image_socket")


@pytest.mark.parametrize("with_wait", [False, True])
def test_shot_waits_for_start_saves_and_restores(worker, tmp_path, with_wait):
    path = compile_shot(tmp_path / "shot.h5", with_wait=with_wait)
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
        saved = file["data/emccd/camera/images"]
        assert saved.attrs["wait_policy"] == "ignore"
        requested = saved["requested_start_times_s"][:]
        sent = saved["trigger_sent_times_s"][:]
        acknowledged = saved["trigger_ack_times_s"][:]
        assert len(sent) == len(acknowledged) == len(requested) == 2
        assert all(acknowledged >= sent)
        assert all(abs(sent - requested) < .1)
        assert saved.attrs["trigger_clock"] == time.get_clock_info("perf_counter").implementation
        assert saved.attrs["start_received_perf_counter_s"] > 0
        assert file["images/side/fluorescence/frame1"].shape == (128, 128)
        sent = file["data/emccd/camera/images/trigger_sent_times_s"][:]
        assert len(sent) == 2 and all(sent >= [.1, .2])
        assert not file["images/side"].attrs["failed_shot"]
        for index in range(2):
            preview = file[f"data/emccd/camera/previews/frame_{index:04d}"]
            assert preview.shape == (128, 128, 3)
            assert preview.dtype == np.uint8
            assert preview.attrs["CLASS"] == b"IMAGE"
            assert preview.attrs["IMAGE_SUBCLASS"] == b"IMAGE_TRUECOLOR"
            assert preview.attrs["INTERLACE_MODE"] == b"INTERLACE_PIXEL"
            png = tmp_path / preview.attrs["png_file"]
            with Image.open(png) as image:
                np.testing.assert_array_equal(np.asarray(image), preview[:])
            assert file[preview.attrs["source_dataset"]].dtype == np.uint16
    assert not Path(str(path) + ".tmp").exists()


def test_preview_repeated_frames_preserves_raw_and_handles_constant_image(tmp_path):
    raw = np.array([[[0, 100], [200, 65535]], [[42, 42], [42, 42]]], dtype=np.uint16)
    exposures = [dict(name="atoms", frametype="frame", t=t, exposure_time_s=.001)
                 for t in (.1, .2)]
    path = tmp_path / "shot.h5"
    with h5py.File(path, "w") as file:
        source = file.create_dataset("data/emccd/camera/images/atoms/frame", data=raw)
        _save_shot_previews(file, "emccd", exposures, path, logging.getLogger("test"))
        np.testing.assert_array_equal(source[:], raw)
        previews = file["data/emccd/camera/previews"]
        assert len(previews) == 2
        first, second = previews["frame_0000"], previews["frame_0001"]
        assert first[0, 0, 0] == 0 and first[1, 1, 0] == 255
        assert not second[:].any()
        assert second.attrs["display_min"] == second.attrs["display_max"] == 42
        assert second.attrs["source_frame_index"] == 1
        for preview in (first, second):
            with Image.open(tmp_path / preview.attrs["png_file"]) as png:
                np.testing.assert_array_equal(np.asarray(png), preview[:])


def test_png_failure_keeps_hdf5_previews_and_raw(tmp_path):
    path = tmp_path / "shot.h5"
    (tmp_path / "emccd_png").write_text("directory cannot be created here")
    exposures = [dict(name="atoms", frametype="frame", t=.1, exposure_time_s=.001)]
    logger = MagicMock()
    with h5py.File(path, "w") as file:
        raw = np.array([[10, 20]], dtype=np.uint16)
        source = file.create_dataset("data/emccd/camera/images/atoms/frame", data=raw)
        _save_shot_previews(file, "emccd", exposures, path, logger)
        np.testing.assert_array_equal(source[:], raw)
        assert file["data/emccd/camera/previews/frame_0000"].shape == (1, 2, 3)
        logger.warning.assert_called_once()


def test_repeated_name_shot_exports_each_frame(worker, tmp_path):
    path = compile_shot(tmp_path / "shot.h5")
    with h5py.File(path, "r+") as file:
        table = file["devices/emccd/EXPOSURES"]
        rows = table[:]
        rows[1]["frametype"] = rows[0]["frametype"]
        table[:] = rows
    worker.prepare_shot_start("repeated")
    worker.transition_to_buffered("emccd", str(path), {}, False)
    worker._start_event.post("repeated")
    assert worker.transition_to_manual()
    with h5py.File(path) as file:
        assert file["images/side/fluorescence/frame0"].shape == (2, 128, 128)
        previews = file["data/emccd/camera/previews"]
        assert len(previews) == 2
        for i in range(2):
            preview = previews[f"frame_{i:04d}"]
            assert preview.attrs["source_frame_index"] == i
            with Image.open(tmp_path / preview.attrs["png_file"]) as png:
                np.testing.assert_array_equal(np.asarray(png), preview[:])


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
    assert not (tmp_path / "emccd_png").exists()


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
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    tab = AndorEMCCDRemoteTab()
    tab._device_name = "emccd"
    tab.settings = {"connection_table": SimpleNamespace(
        find_by_name=lambda _: SimpleNamespace(properties={})), "front_panel_settings": {
            "~display/intensity_min": {"base_value": 100},
            "~preview/max_rate_hz": {"base_value": 10}}}
    widget = QtWidgets.QWidget()
    layout = QtWidgets.QVBoxLayout(widget)
    tab.get_tab_layout = lambda: layout
    tab.supports_smart_programming = MagicMock()
    tab.statemachine_timeout_add = MagicMock()
    tab.event_queue = MagicMock()
    tab.initialise_GUI()
    assert not hasattr(tab, "image")
    assert not hasattr(tab, "image_receiver")
    assert not any(key.startswith(("~display/", "~preview/"))
                   for key in tab.settings["front_panel_settings"])
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
    tab.restore_save_data({"camera_panel": {}, "connection_connected": True,
                           "acquiring": True, "colormap": "legacy", "max_rate": 20})
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


def test_display_metadata_preserves_fractional_bg_and_raw():
    from AndorEMCCDRemote.display_metadata import frame_display_metadata
    raw = np.array([[100, 101], [102, 103]], dtype=np.uint16)
    config = dict(units="photons", source="manual", sensitivity_e_per_adu=2,
                  em_gain=10, qe=.8, baseline_adu=100, background_subtract_enabled=True)
    snapshot = {"profiles": {"gui": {"profile": {
        "display": {"conversion": config},
        "analysis": {"rois": [dict(x=0, y=0, width=2, height=1,
                                   enabled=True, background=True)]}}}}}
    before = raw.copy()
    metadata = frame_display_metadata(raw, snapshot)["gui"]
    assert metadata["background"]["mean"] == 100.5
    assert metadata["conversion"]["offset"] == 100.5  # no second bias subtraction
    assert metadata["conversion"]["scale"] == .25
    np.testing.assert_array_equal(before, raw)


def test_shot_snapshots_remote_gui_and_actual_shot_calibration(worker, tmp_path):
    import json
    worker.connect_to_device()
    client = worker.camera._client
    client.request("set_display_profile", {"profile_id": "test-remote-gen0", "profile": {
        "host": "gen0", "display": {"conversion": dict(units="electrons", source="camera",
            wavelength_nm=780, background_subtract_enabled=True)},
        "analysis": {"rois": [dict(x=0, y=0, width=2, height=2,
                                   enabled=True, background=True)]},
        "calibration": {"em_gain": 9999}}})
    path = compile_shot(tmp_path / "display.h5")
    worker.prepare_shot_start("display-test")
    worker.transition_to_buffered("emccd", str(path), {}, False)
    worker._start_event.post("display-test")
    assert worker.transition_to_manual()
    with h5py.File(path) as file:
        data = file["data/emccd/camera/images"]
        profiles = json.loads(data["gui_settings_snapshot"][()])["profiles"]
        assert profiles["test-remote-gen0"]["profile"]["host"] == "gen0"
        assert profiles["test-remote-gen0"]["shot_calibration"]["em_gain"] != 9999
        frames = data["frame_display_metadata"][:]
        assert len(frames) == 2
        metadata = json.loads(frames[0])["test-remote-gen0"]
        raw = data["fluorescence/frame0"][:]
        assert raw.dtype == np.uint16
        assert metadata["background"]["mean"] == float(raw[:2, :2].mean())
        assert metadata["conversion"]["valid"]
        assert json.loads(data.attrs["camera_settings"])["count_convert_mode"] == 0

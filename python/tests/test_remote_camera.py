"""Tests AndorEMCCDRemoteCamera against a running (simulated) backend.

No labscript installation is needed. Skipped if the backend is unreachable.
Endpoint selection (environment variables):
    AEC_TEST_HOST (default localhost), AEC_TEST_CONTROL_PORT (47100),
    AEC_TEST_DATA_PORT (47101), AEC_TEST_IPC_DIR (/run/user/1000/aec).

Run:  python3 -m pytest -v python/tests/test_remote_camera.py
"""

from __future__ import annotations

import os
import sys
import threading
import time

import numpy as np
import pytest

_PYTHON_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _PYTHON_DIR)
sys.path.insert(0, os.path.join(_PYTHON_DIR, "labscript_devices"))

from andor_emccd import AndorEMCCDClient  # noqa: E402
from AndorEMCCDRemote.blacs_workers import AndorEMCCDRemoteCamera  # noqa: E402

_KWARGS = dict(
    host=os.environ.get("AEC_TEST_HOST", "localhost"),
    control_port=int(os.environ.get("AEC_TEST_CONTROL_PORT", "47100")),
    data_port=int(os.environ.get("AEC_TEST_DATA_PORT", "47101")),
    ipc_dir=os.environ.get("AEC_TEST_IPC_DIR", "/run/user/1000/aec"),
)


def _backend_settings():
    try:
        with AndorEMCCDClient(timeout_s=2.0, **_KWARGS) as client:
            if not client.ping().get("simulated", False):
                return None  # never run these tests on real hardware
            return client.get_settings()
    except Exception:  # noqa: BLE001
        return None


_ORIGINAL = _backend_settings()
pytestmark = pytest.mark.skipif(
    _ORIGINAL is None, reason="simulated backend not reachable")


@pytest.fixture
def camera():
    cam = AndorEMCCDRemoteCamera(poll_interval_s=0.2, name="pytest",
                                 force_control=True, **_KWARGS)
    cam._ensure_control()
    cam._client.stop()
    cam.set_attributes({"exposure_time_s": 0.001, "em_gain_enabled": False,
                        "em_gain": 0,
                        "hbin": 1, "vbin": 1, "hstart": 1, "hend": 0,
                        "vstart": 1, "vend": 0, "isolated_crop": False})
    yield cam
    cam._client.stop()
    cam._client.acquire_control(force=True)
    cam._client.set_settings(_ORIGINAL)
    cam.close()


def test_em_gain_switch(camera):
    status = camera._client.get_status
    camera.set_attributes({"em_gain": 50})
    assert status()["em_gain"] == 0  # stored, but EM gain is off
    camera.set_attributes({"em_gain_enabled": True})
    assert status()["em_gain"] == 50
    assert "em_gain_enabled" in camera.get_attribute_names("simple")
    camera.set_attributes({"em_gain_enabled": False})
    assert status()["em_gain"] == 0
    assert camera.get_attribute("em_gain") == 50


def test_attributes(camera):
    camera.set_attributes({"exposure_time_s": 0.005, "em_gain": 12})
    assert camera.get_attribute("exposure_time_s") == pytest.approx(0.005)
    assert camera.get_attribute("em_gain") == 12
    simple = camera.get_attribute_names("simple")
    intermediate = camera.get_attribute_names("intermediate")
    advanced = camera.get_attribute_names("advanced")
    assert "exposure_time_s" in simple
    assert set(simple) < set(intermediate) < set(advanced)
    attrs = camera.get_attributes("intermediate")
    assert list(attrs) == intermediate
    with pytest.raises(KeyError):
        camera.get_attribute("no_such_attribute")


def test_snap_software_trigger(camera):
    camera.set_attributes({"trigger_mode": "software"})
    image = camera.snap()
    assert image.shape == (512, 512)
    assert image.dtype == np.uint16
    assert image.mean() > 0


def test_grab_multiple_fills_incrementally(camera):
    # External triggers arrive at AEC_SIM_TRIGGER_HZ (default 10 Hz).
    n = 6
    camera.set_attributes({"trigger_mode": "external"})
    camera.configure_acquisition(continuous=False, bufferCount=n)
    images = []
    thread = threading.Thread(target=camera.grab_multiple,
                              args=(n, images), daemon=True)
    thread.start()
    seen = set()
    deadline = time.monotonic() + 10.0
    while thread.is_alive() and time.monotonic() < deadline:
        seen.add(len(images))
        time.sleep(0.01)
    thread.join(timeout=1.0)
    assert not thread.is_alive()
    assert len(images) == n
    assert all(img.shape == (512, 512) for img in images)
    # Partial fills were observed while the shot was running.
    assert any(0 < k < n for k in seen), seen
    assert [h["index"] for h in camera.last_headers] == list(range(n))
    camera.stop_acquisition()


def test_abort(camera):
    camera.set_attributes({"trigger_mode": "external"})
    camera.configure_acquisition(continuous=False, bufferCount=100)
    images = []
    thread = threading.Thread(target=camera.grab_multiple,
                              args=(100, images), daemon=True)
    thread.start()
    time.sleep(0.5)
    t0 = time.monotonic()
    camera.abort_acquisition()
    thread.join(timeout=3.0)
    assert not thread.is_alive()
    assert time.monotonic() - t0 < 2.0
    assert len(images) < 100
    assert camera._client.get_status()["state"] != "acquiring"


def test_failed_shot_raises_when_run_ends_short(camera):
    camera.set_attributes({"trigger_mode": "external"})
    camera.configure_acquisition(continuous=False, bufferCount=100)
    errors = []

    def target():
        try:
            camera.grab_multiple(100, [])
        except RuntimeError as e:
            errors.append(e)

    thread = threading.Thread(target=target, daemon=True)
    thread.start()
    time.sleep(0.3)
    camera._client.stop()  # someone else stops the run
    thread.join(timeout=3.0)
    assert not thread.is_alive()
    assert errors and "ended after" in str(errors[0])


def test_continuous_grab(camera):
    camera.set_attributes({"trigger_mode": "internal"})
    camera.configure_acquisition(continuous=True)
    seqs = []
    for _ in range(3):
        image = camera.grab()
        assert image.shape == (512, 512)
        seqs.append(camera.last_headers[0]["seq"])
    camera.stop_acquisition()
    assert seqs == sorted(set(seqs))

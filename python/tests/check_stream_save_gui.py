"""Linux offscreen GUI save check against an isolated simulated backend.

PYTHONPATH=python python3 python/tests/check_stream_save_gui.py
Requires tifffile; backend port 47400. Uses temporary preferences.
"""
import os
from pathlib import Path
import subprocess
import tempfile

import numpy as np
from andor_emccd import AndorEMCCDClient
from andor_emccd.recording import iter_recorded_frames


def main():
    root = Path(__file__).resolve().parents[2]
    output_dir = root / 'build' / 'stream-test'
    output_dir.mkdir(parents=True, exist_ok=True)
    output = output_dir / 'live-save.tif'
    output.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix='aec-save-gui-') as config_dir:
        prefs = Path(config_dir) / 'AndorEMCCDcontroller' / 'AndorEMCCDcontroller.conf'
        prefs.parent.mkdir(parents=True)
        prefs.write_text('[connection]\nbuffer_frames=64\n[display]\naverage_frames=3\n')
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', XDG_CONFIG_HOME=config_dir)
        with AndorEMCCDClient('localhost', control_port=47400, ipc=False) as camera:
            assert camera.ping()['simulated'], 'isolated simulated backend required'
            camera.acquire_control()
            camera.start({'acquisition_mode': 'continuous', 'trigger_mode': 'internal',
                          'hbin': 16, 'vbin': 16, 'exposure_time_s': 0.02})
            camera.release_control()
            try:
                subprocess.run([str(root / 'build/exec/gui/andor_emccd_gui'),
                                '--tcp', '--host', 'localhost', '--control-port', '47400',
                                '--data-port', '47401', '--stream-save', str(output),
                                '--save-max-fps', '5', '--screenshot',
                                str(output_dir / 'stream-save.png'),
                                '--screenshot-delay', '2500'],
                               env=env, timeout=15, check=True)
            finally:
                camera.acquire_control()
                camera.stop()
    frames = list(iter_recorded_frames(output))
    assert len(frames) >= 8, len(frames)
    assert all(a.dtype == np.uint16 for a, m in frames)
    timestamps = [m['frame']['timestamp'] for a, m in frames]
    assert all(b - a >= 0.2 - 1e-6 for a, b in zip(timestamps, timestamps[1:]))
    assert all(m['recording']['max_fps'] == 5 for a, m in frames)
    assert frames[-1][1]['recording']['rate_skipped_total'] > 0
    assert frames[-1][1]['recording']['queue_dropped_total'] == 0
    print(f'GUI streaming save: {len(frames)} raw uint16 frames, <=5 fps; display average=3')


if __name__ == '__main__':
    main()

"""ADU per electron vs EM gain setting (real-gain mode 3 and DAC mode 0), shutter closed.

  python3 bench/em_scan.py OUT.json
"""
import sys, json, numpy as np
sys.path.insert(0, __file__.rsplit("/bench/", 1)[0])
from andor_emccd import AndorEMCCDClient
cam = AndorEMCCDClient("localhost", name="em-scan", acquire=True)
base = {"acquisition_mode": "kinetic", "trigger_mode": "internal", "frame_transfer": False,
        "hbin": 1, "vbin": 1, "hstart": 1, "hend": 512, "vstart": 1, "vend": 512,
        "isolated_crop": False, "shutter_mode": "closed", "exposure_time_s": 2.0}
img, _ = cam.acquire(10, timeout_s=60, settings=dict(base, exposure_time_s=0.01, em_gain_enabled=False))
f = img.astype(float)[:, 64:448, 64:448]
read = float(np.std(np.diff(f, axis=0)) / np.sqrt(2))
out = []
for mode, gain in ((3, 50), (3, 100), (3, 200), (3, 300), (0, 255)):
    rep = cam.set_settings(dict(base, em_gain_mode=mode, em_gain=gain, em_gain_enabled=True))
    img, _ = cam.acquire(15, timeout_s=90, settings=dict(base, em_gain_mode=mode, em_gain=gain, em_gain_enabled=True))
    f = img.astype(float)[:, 64:448, 64:448]
    x = f - np.median(f, axis=(1, 2), keepdims=True)
    thr = 5 * read
    ev = x > thr
    m = float(x.mean()); v = float(np.var(np.diff(f, axis=0)) / 2 - read ** 2)
    r = {"mode": mode, "gain": gain, "warnings": rep.get("warnings"), "events": float(ev.mean()),
         "tail_adu_per_e": float(x[ev].mean() - thr), "var_adu_per_e": v / (2 * m) if m > 0 else None,
         "p999_adu": float(np.percentile(x, 99.9))}
    out.append(r); print(r, flush=True)
cam.set_settings({"em_gain_enabled": False, "em_gain_mode": 0, "em_gain": 30, "acquisition_mode": "single",
                  "exposure_time_s": 0.01, "frame_transfer": True, "shutter_mode": "auto"})
json.dump({"read_adu": read, "points": out}, open(sys.argv[1], "w"), indent=1)
cam.release_control()

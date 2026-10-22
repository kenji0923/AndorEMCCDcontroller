"""Dark current by counting EM single-electron events (keep the camera capped).

  python3 bench/em_count.py LABEL EM_GAIN OUT.json [SHUTTER=auto] [EM_GAIN_MODE=3]

Events: pixels > 5 x read noise above the frame median. ADU per electron from
the exponential tail; counts are corrected by the detected fraction
exp(-threshold / ADU per electron). Slope vs exposure = dark e-/pixel/s,
intercept = clock-induced charge per frame.
"""
import json, sys, numpy as np
sys.path.insert(0, __file__.rsplit("/bench/", 1)[0])
from andor_emccd import AndorEMCCDClient
label, gain, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
shutter = sys.argv[4] if len(sys.argv) > 4 else "auto"
mode = int(sys.argv[5]) if len(sys.argv) > 5 else 3
cam = AndorEMCCDClient("localhost", name="em-count", acquire=True)
base = {"acquisition_mode": "kinetic", "trigger_mode": "internal", "frame_transfer": False,
        "hbin": 1, "vbin": 1, "hstart": 1, "hend": 512, "vstart": 1, "vend": 512,
        "isolated_crop": False, "kinetic_cycle_time_s": 0.0, "shutter_mode": shutter}
# Read noise and bias without EM gain (EM amplifier, same readout).
img, _ = cam.acquire(20, timeout_s=60, settings=dict(base, exposure_time_s=0.01, em_gain_enabled=False))
f = img.astype(float)[:, 64:448, 64:448]
bias = float(np.median(f)); read = float(np.median([np.std(np.diff(f, axis=0)[i]) for i in range(19)]) / np.sqrt(2))
rep = cam.set_settings(dict(base, em_gain_mode=mode, em_gain=gain, em_gain_enabled=True))
print("warnings", rep.get("warnings"), "bias", round(bias, 2), "read", round(read, 3))
res = {"label": label, "shutter_mode": shutter, "em_gain_mode": mode, "bias_adu": bias, "read_noise_adu": read, "em_gain_set": gain, "points": []}
for t, n in ((0.001, 50), (2.0, 20), (5.0, 12), (10.0, 8)):
    img, _ = cam.acquire(n, timeout_s=n * (t + 0.2) + 30,
                         settings=dict(base, exposure_time_s=t, em_gain_mode=mode, em_gain=gain, em_gain_enabled=True))
    st = cam.get_status()
    f = img.astype(float)[:, 64:448, 64:448]
    b = np.median(f, axis=(1, 2), keepdims=True)  # per-frame baseline
    x = f - b
    thr = 5 * read
    ev = x > thr
    rate = float(ev.mean())                       # events / pixel / frame
    adu_per_e = float(x[ev].mean() - thr) if ev.any() else float("nan")  # exponential tail
    m = float(x.mean()); v = float(np.var(np.diff(f, axis=0)) / 2 - read ** 2)
    det = float(np.exp(-thr / adu_per_e)) if adu_per_e > 0 else float("nan")
    p = {"detected_fraction": det, "electrons_per_pixel": rate / det, "mean_excess_adu": m, "var_excess_adu2": v,
         "adu_per_e_from_var": v / (2 * m) if m > 0 else float("nan"),
         "exposure_s": t, "n": n, "events_per_pixel": rate, "adu_per_electron": adu_per_e,
         "em_gain_status": st.get("em_gain"), "temperature_c": st["temperature_c"]}
    res["points"].append(p)
    print(p, flush=True)
t = np.array([p["exposure_s"] for p in res["points"]]); r = np.array([p["electrons_per_pixel"] for p in res["points"]])
slope, cic = np.polyfit(t, r, 1)
res["dark_e_per_pixel_s"] = float(slope); res["cic_e_per_pixel_frame"] = float(cic)
print("dark %.5f e/pix/s  CIC+offset %.5f e/pix/frame" % (slope, cic))
cam.set_settings({"em_gain_enabled": False, "em_gain_mode": 0, "em_gain": 30, "acquisition_mode": "single",
                  "exposure_time_s": 0.01, "frame_transfer": True, "shutter_mode": "auto"})
json.dump(res, open(out, "w"), indent=1)
cam.release_control()

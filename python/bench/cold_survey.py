#!/usr/bin/env python3
"""Dark-frame survey of a cooled sensor under many readout settings.

Keep the camera capped; the shutter is kept closed. The cooler is not changed
(run cool_down.py first). Sub-commands, each appending to one JSON file:

  readout  bias and read noise (EM gain off) for every amplifier, horizontal
           speed and pre-amplifier gain
  cic      clock-induced charge from 1 ms frames with EM gain, for every
           vertical shift speed x clock amplitude, and for every EM
           horizontal speed
  gain     ADU per electron vs EM gain setting (DAC 0-255 and real gain modes)
  dark     dark current from 0.001-10 s frames with EM gain (short version of
           em_count.py)

Single-electron events are pixels more than 5 x read noise above the frame
median; ADU/e- comes from the exponential tail and the counts are corrected by
the detected fraction exp(-threshold / ADU per electron).

  python3 bench/cold_survey.py results/cold_survey_m80.json readout cic gain dark
"""

import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, __file__.rsplit("/bench/", 1)[0])
from andor_emccd import AndorEMCCDClient  # noqa: E402

CROP = (slice(64, 448), slice(64, 448))
BASE = {"acquisition_mode": "kinetic", "trigger_mode": "internal",
        "frame_transfer": False, "hbin": 1, "vbin": 1, "hstart": 1,
        "hend": 512, "vstart": 1, "vend": 512, "isolated_crop": False,
        "kinetic_cycle_time_s": 0.0, "number_accumulations": 1,
        "shutter_mode": "closed", "baseline_clamp": True,
        "output_amplifier": 0, "hs_speed_index": 0, "preamp_gain_index": 0,
        "vs_speed_index": -1, "vs_amplitude": 0}
# Recommended EM readout: 10 MHz, pre-amp 1 (index 0), EM DAC mode at the
# highest gain allowed at the current temperature (the backend clamps it).
EM = {"em_gain_enabled": True, "em_gain_mode": 0, "em_gain": 255}


def grab(cam, settings, exposure_s, n):
    s = dict(BASE, **settings, exposure_time_s=exposure_s)
    reply = cam.set_settings(s)
    img, _ = cam.acquire(n, timeout_s=n * (exposure_s + 0.3) + 30, settings=s)
    st = cam.get_status()
    return (img.astype(np.float64)[:, CROP[0], CROP[1]], st,
            reply.get("warnings", []))


def read_noise(frames):
    d = np.diff(frames, axis=0) / np.sqrt(2.0)
    return float(np.median(np.std(d, axis=(1, 2))))


def events(frames, read):
    x = frames - np.median(frames, axis=(1, 2), keepdims=True)
    thr = 5.0 * read
    ev = x > thr
    rate = float(ev.mean())
    g = float(x[ev].mean() - thr) if ev.any() else float("nan")
    det = float(np.exp(-thr / g)) if g > 0 else float("nan")
    # Mean excess / gain does not saturate when pixels hold several electrons.
    return {"events_per_pixel": rate, "adu_per_electron": g,
            "mean_excess_adu": float(x.mean()),
            "detected_fraction": det,
            "electrons_per_pixel": rate / det if det > 0 else float("nan")}


def common(st, warnings):
    return {"temperature_c": st["temperature_c"],
            "temperature_status": st["temperature_status"],
            "em_gain_in_effect": st.get("em_gain"),
            "timings": st.get("timings"), "warnings": warnings}


def survey_readout(cam, info):
    out = []
    for amp, a in enumerate(info["amplifiers"]):
        for hs, mhz in enumerate(a["hs_speeds_mhz"]):
            for pa, pg in enumerate(info["preamp_gains"]):
                f, st, w = grab(cam, {"output_amplifier": amp,
                                      "hs_speed_index": hs,
                                      "preamp_gain_index": pa,
                                      "em_gain_enabled": False}, 0.001, 20)
                p = dict(amplifier=a["name"], hs_mhz=mhz, preamp=pg,
                         bias_adu=float(np.median(f)), read_noise_adu=read_noise(f),
                         **common(st, w))
                out.append(p)
                print(f"  {a['name'][:12]:12s} {mhz:4.0f} MHz pre {pg:3.1f}: "
                      f"bias {p['bias_adu']:7.1f}  read {p['read_noise_adu']:6.3f} ADU"
                      f"  {w}", flush=True)
    return out


def em_point(cam, settings, exposure_s, n, read):
    f, st, w = grab(cam, dict(EM, **settings), exposure_s, n)
    return dict(exposure_s=exposure_s, n=n, read_noise_adu=read,
                **events(f, read), **common(st, w))


def read_for(cam, settings):
    f, _, _ = grab(cam, dict(settings, em_gain_enabled=False), 0.001, 20)
    return read_noise(f)


def survey_cic(cam, info):
    out = {"vertical": [], "horizontal": []}
    read = read_for(cam, {})
    for vs, us in enumerate(info["vs_speeds_us"]):
        for amp, name in enumerate(info["vs_amplitudes"]):
            p = em_point(cam, {"vs_speed_index": vs, "vs_amplitude": amp},
                         0.001, 40, read)
            p.update(vs_us=us, vs_amplitude=name)
            out["vertical"].append(p)
            print(f"  vs {us:4.2f} us amp {name:6s}: CIC "
                  f"{p['electrons_per_pixel']:.4f} e/pix  "
                  f"{p['adu_per_electron']:5.1f} ADU/e  {p['warnings']}",
                  flush=True)
    for hs, mhz in enumerate(info["amplifiers"][0]["hs_speeds_mhz"]):
        r = read_for(cam, {"hs_speed_index": hs})
        p = em_point(cam, {"hs_speed_index": hs}, 0.001, 40, r)
        p.update(hs_mhz=mhz)
        out["horizontal"].append(p)
        print(f"  hs {mhz:4.0f} MHz: read {r:.3f} ADU  CIC "
              f"{p['electrons_per_pixel']:.4f} e/pix  "
              f"{p['adu_per_electron']:5.1f} ADU/e", flush=True)
    return out


def survey_gain(cam, info):
    out = []
    read = read_for(cam, {})
    scans = ((0, (10, 20, 30, 40, 50, 60, 71)),
             (3, (50, 100, 200, 300, 500, 1000)))
    for mode, values in scans:
        for g in values:
            p = em_point(cam, {"em_gain_mode": mode, "em_gain": g}, 2.0, 15,
                         read)
            p.update(em_gain_mode=mode, em_gain_set=g)
            out.append(p)
            print(f"  mode {mode} set {g:5d} (in effect "
                  f"{p['em_gain_in_effect']}): {p['adu_per_electron']:6.2f} "
                  f"ADU/e  {p['warnings']}", flush=True)
    return out


def survey_dark(cam, info):
    read = read_for(cam, {})
    pts = [em_point(cam, {}, t, n, read)
           for t, n in ((0.001, 40), (2.0, 15), (5.0, 10), (10.0, 6))]
    t = np.array([p["exposure_s"] for p in pts])
    r = np.array([p["electrons_per_pixel"] for p in pts])
    slope, cic = np.polyfit(t, r, 1)
    print(f"  dark {slope:.5f} e/pix/s  CIC {cic:.4f} e/pix/frame  "
          f"({pts[-1]['adu_per_electron']:.1f} ADU/e, "
          f"{pts[-1]['temperature_c']:.2f} C)", flush=True)
    return {"dark_e_per_pixel_s": float(slope),
            "cic_e_per_pixel_frame": float(cic), "points": pts}


def main() -> int:
    path, tasks = sys.argv[1], sys.argv[2:]
    cam = AndorEMCCDClient("localhost", name="cold-survey", acquire=True)
    info = cam.get_info()
    res = json.load(open(path)) if os.path.exists(path) else {}
    st = cam.get_status()
    print(f"{st['temperature_c']:.2f} C ({st['temperature_status']})",
          flush=True)
    try:
        for task in tasks:
            print(task, flush=True)
            res[task] = globals()["survey_" + task](cam, info)
            res[task + "_time"] = time.strftime("%Y-%m-%d %H:%M:%S")
            json.dump(res, open(path, "w"), indent=1)
    finally:
        cam.set_settings({"em_gain_enabled": False, "em_gain_mode": 0,
                          "em_gain": 30, "acquisition_mode": "single",
                          "exposure_time_s": 0.01, "frame_transfer": True,
                          "shutter_mode": "auto", "output_amplifier": 0,
                          "hs_speed_index": 0, "preamp_gain_index": 0,
                          "vs_speed_index": -1, "vs_amplitude": 0})
        cam.release_control()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

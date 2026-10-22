#!/usr/bin/env python3
"""Dark level, dark current and noise of the sensor at its current temperature.

Takes kinetic series of dark frames (keep the camera capped) at several
exposure times with the EM gain off, and a few with the EM gain on, then
reports per exposure:
  level      median pixel value of the frame average (ADU)
  noise      robust std of (frame[i+1] - frame[i]) / sqrt(2) (ADU), i.e.
             temporal noise without the fixed pattern
  hot        fraction of pixels above level + 10 * noise / sqrt(n_frames)
             in the frame average
and fits the dark current (ADU/pixel/s) from level vs exposure. The cooler
and temperature are not changed; run cool_down.py first for a cold point.

  python3 bench/dark_noise.py --label warm --json-out bench/results/dark_warm.json
"""

import argparse
import json
import math
import sys
import time

import numpy as np

sys.path.insert(0, __file__.rsplit("/bench/", 1)[0])
from andor_emccd import AndorEMCCDClient  # noqa: E402

# Central region: avoids the edges and the overscan-like first columns.
CROP = (slice(64, 448), slice(64, 448))


def robust_std(x: np.ndarray) -> float:
    med = np.median(x)
    return float(1.4826 * np.median(np.abs(x - med)))


def measure(cam, exposure_s, n_frames, em_gain):
    settings = {
        "acquisition_mode": "kinetic",
        "trigger_mode": "internal",
        "exposure_time_s": exposure_s,
        # Without frame transfer the exposure is exactly the set value.
        "frame_transfer": False,
        "kinetic_cycle_time_s": 0.0,
        "number_accumulations": 1,
        "hbin": 1, "vbin": 1, "hstart": 1, "hend": 512, "vstart": 1,
        "vend": 512, "isolated_crop": False,
        "em_gain_enabled": em_gain > 0,
        "em_gain": max(em_gain, 0),
    }
    reply = cam.set_settings(settings)
    timeout = n_frames * (exposure_s + 0.1) + 30
    images, headers = cam.acquire(n_frames, timeout_s=timeout,
                                  settings=settings)
    status = cam.get_status()
    frames = images.astype(np.float64)[:, CROP[0], CROP[1]]
    avg = frames.mean(axis=0)
    level = float(np.median(avg))
    diffs = np.diff(frames, axis=0) / math.sqrt(2.0)
    noise = float(np.median([robust_std(d) for d in diffs]))
    noise_std = float(np.median([d.std() for d in diffs]))
    threshold = level + 10.0 * noise / math.sqrt(n_frames)
    hot = float(np.mean(avg > threshold))
    timings = status.get("timings", {})
    return {
        "exposure_s": exposure_s,
        "exposure_actual_s": timings.get("exposure_s"),
        "em_gain": em_gain,
        "em_gain_in_effect": status.get("em_gain"),
        "n_frames": n_frames,
        "level_adu": level,
        "mean_adu": float(avg.mean()),
        "noise_adu": noise,
        "noise_std_adu": noise_std,
        "fixed_pattern_adu": robust_std(avg),
        "hot_fraction": hot,
        "temperature_c": status.get("temperature_c"),
        "temperature_status": status.get("temperature_status"),
        "warnings": reply.get("warnings", []),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--label", required=True)
    parser.add_argument("--json-out")
    parser.add_argument("--frames", type=int, default=6)
    parser.add_argument("--exposures", type=float, nargs="+",
                        default=[0.01, 0.1, 0.5, 1.0, 2.0, 5.0])
    parser.add_argument("--em-gain", type=int, default=100)
    parser.add_argument("--em-exposures", type=float, nargs="+",
                        default=[0.01, 1.0])
    args = parser.parse_args()

    cam = AndorEMCCDClient(args.host, name="dark-noise", acquire=True)
    start = cam.get_status()
    print(f"{args.label}: {start['temperature_c']:.2f} C "
          f"({start['temperature_status']}, cooler "
          f"{'on' if start['cooler_on'] else 'off'})", flush=True)
    points = []
    for gain, exposures in ((0, args.exposures),
                            (args.em_gain, args.em_exposures)):
        for exposure in exposures:
            p = measure(cam, exposure, args.frames, gain)
            points.append(p)
            print(f"  gain {gain:3d} t {exposure:6.3f} s: level "
                  f"{p['level_adu']:8.2f}  noise {p['noise_adu']:6.2f}  "
                  f"(std {p['noise_std_adu']:6.2f})  FPN "
                  f"{p['fixed_pattern_adu']:6.2f}  hot "
                  f"{100 * p['hot_fraction']:.3f} %  T "
                  f"{p['temperature_c']:.2f}", flush=True)
    # Back to EM gain off and a short exposure.
    cam.set_settings({"em_gain_enabled": False, "acquisition_mode": "single",
                      "exposure_time_s": 0.01, "frame_transfer": True})

    base = [p for p in points if p["em_gain"] == 0]
    t = np.array([p["exposure_s"] for p in base])
    level = np.array([p["level_adu"] for p in base])
    slope, offset = np.polyfit(t, level, 1)
    result = {
        "label": args.label,
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
        "start_status": {k: start.get(k) for k in
                         ("temperature_c", "temperature_status", "cooler_on")},
        "dark_current_adu_per_pixel_s": float(slope),
        "bias_adu": float(offset),
        "read_noise_adu": base[0]["noise_adu"],
        "points": points,
    }
    print(f"  dark current {slope:.4f} ADU/pixel/s, bias {offset:.2f} ADU, "
          f"read noise {base[0]['noise_adu']:.2f} ADU")
    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump(result, f, indent=1)
    cam.release_control()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

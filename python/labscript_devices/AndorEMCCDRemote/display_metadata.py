"""Snapshot independent GUI profiles and measure BG on unchanged shot pixels."""
import copy
import math
import time

import numpy as np


def capture_display_profiles(client):
    snapshot = {"captured_at": time.time(), "profiles": {},
                "capture_phase": "transition_to_buffered after camera settings"}
    try:
        snapshot["profiles"] = copy.deepcopy(client.request("get_display_profiles"))
        snapshot["status"] = "ok"
        for entry in snapshot["profiles"].values():
            config = entry.get("profile", {}).get("display", {}).get("conversion", {})
            entry["age_s_at_capture"] = max(0, snapshot["captured_at"] - entry["updated_at"])
            try:
                entry["shot_calibration"] = client.request("get_calibration", {
                    "wavelength_nm": config.get("wavelength_nm", 780.0)})
            except Exception as error:
                entry["shot_calibration"] = {"error": str(error)}
    except Exception as error:
        snapshot["status"] = "unavailable"
        snapshot["error"] = str(error)
    return snapshot


def frame_display_metadata(raw, snapshot):
    result = {}
    for profile_id, entry in snapshot.get("profiles", {}).items():
        profile = entry.get("profile", {})
        config = profile.get("display", {}).get("conversion", {})
        background = {}
        for roi in profile.get("analysis", {}).get("rois", []):
            if not roi.get("enabled", True) or not roi.get("background", False):
                continue
            x, y = int(roi["x"]), int(roi["y"])
            x0, y0 = max(0, x), max(0, y)
            x1 = min(raw.shape[1], x + int(roi["width"]))
            y1 = min(raw.shape[0], y + int(roi["height"]))
            pixels = raw[y0:max(y0, y1), x0:max(x0, x1)]
            mean = float(pixels.mean(dtype=np.float64)) if pixels.size else None
            background = {"roi": roi, "estimator": "mean", "count": int(pixels.size),
                          "sum": float(pixels.sum(dtype=np.float64)), "mean": mean,
                          "valid": mean is not None and math.isfinite(mean)}
            break
        # Camera coefficients are re-read for this shot's actual settings,
        # not copied from the GUI's earlier live acquisition.
        values = entry.get("shot_calibration", {}) if config.get("source", "camera") == "camera" else config
        units = config.get("units", "counts")
        conversion = {"requested_units": units, "valid": False,
                      "formula": "(raw - offset) * scale", "raw_pixels_modified": False}
        try:
            offset, scale = 0.0, 1.0
            if units not in ("counts", "electrons", "photons"):
                raise ValueError("Unknown units")
            if units != "counts":
                sensitivity, gain = float(values["sensitivity_e_per_adu"]), float(values["em_gain"])
                if not (math.isfinite(sensitivity) and sensitivity > 0 and math.isfinite(gain) and gain > 0):
                    raise ValueError("Invalid sensitivity or physical EM gain")
                scale = sensitivity / gain
                offset = values.get("baseline_adu")
                if units == "photons":
                    qe = float(values["qe"])
                    if not 0 < qe <= 1:
                        raise ValueError("Invalid quantum efficiency")
                    scale /= qe
            if config.get("background_subtract_enabled", False):
                if not background.get("valid", False):
                    raise ValueError("Background ROI unavailable")
                offset = background["mean"]
            offset = float(offset)
            if not (math.isfinite(offset) and math.isfinite(scale) and scale > 0):
                raise ValueError("Invalid offset or scale")
            conversion.update(valid=True, units=units, offset=offset, scale=scale)
        except (KeyError, ValueError, TypeError, ZeroDivisionError) as error:
            conversion["error"] = str(error)
        result[profile_id] = {"background": background, "conversion": conversion}
    return result

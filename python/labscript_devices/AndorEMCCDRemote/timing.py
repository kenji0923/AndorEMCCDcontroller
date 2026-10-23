"""Shared compile-time and runtime validation for software-triggered shots."""

import math

SOFTWARE_TRIGGER_QUANTUM_S = 1e-4


def validate_start_time(t):
    t = float(t)
    if not math.isfinite(t) or t < 0:
        raise ValueError("Exposure start time must be finite and non-negative")
    nearest = round(t / SOFTWARE_TRIGGER_QUANTUM_S) * SOFTWARE_TRIGGER_QUANTUM_S
    # Only forgive floating-point arithmetic noise, never quantise a request.
    if not math.isclose(t, nearest, rel_tol=0, abs_tol=1e-12):
        raise ValueError(
            f"Exposure start time {t:.15g} s must be a multiple of "
            "0.1 ms (100 us) for software triggering")
    return t


def validate_exposures(exposures, recovery_time=0.0, stop_time=None):
    """Validate rows with t/exposure_time_s fields; return time-sorted rows."""
    rows = sorted(exposures, key=lambda row: float(row["t"]))
    duration = None
    previous = None
    for row in rows:
        t = validate_start_time(row["t"])
        exposure = float(row["exposure_time_s"])
        if not math.isfinite(exposure) or exposure <= 0:
            raise ValueError("Exposure time must be finite and positive")
        if duration is not None and not math.isclose(
                exposure, duration, rel_tol=0, abs_tol=1e-12):
            raise ValueError("All exposures in one software-triggered shot must "
                             "use the same exposure_time")
        duration = exposure
        if previous is not None and t < previous + max(exposure, recovery_time) - 1e-12:
            raise ValueError("Exposures overlap or violate the camera recovery time")
        if stop_time is not None and t + exposure > stop_time + 1e-12:
            raise ValueError("Exposure extends beyond the shot stop time")
        previous = t
    return rows

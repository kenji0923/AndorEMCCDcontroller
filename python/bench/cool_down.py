#!/usr/bin/env python3
"""Turns the cooler on and follows the sensor down to the target temperature.

Safety: the cooler is switched off again (and the script exits with 1) if
  - the sensor gets warmer than start + 3 C (from a cold start, below 0 C:
    the higher of start + 3 C and target + 15 C, to allow the thermal lag and
    overshoot of the controller), or
  - when cooling, it has not cooled by at least --min-drop C after
    --check-after-s (e.g. no coolant flow and the head is overheating).
A target above the current temperature (stepping up) is allowed.
Stops when the SDK reports "stabilized" for --hold-s, or after --timeout-s.
Prints and optionally writes the temperature log (time, T, status).

  python3 bench/cool_down.py --target -80 --json-out bench/results/cool_down.json
"""

import argparse
import json
import sys
import time

sys.path.insert(0, __file__.rsplit("/bench/", 1)[0])
from andor_emccd import AndorEMCCDClient  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--target", type=int, required=True)
    parser.add_argument("--fan", default="full")
    parser.add_argument("--interval-s", type=float, default=10.0)
    parser.add_argument("--check-after-s", type=float, default=180.0)
    parser.add_argument("--min-drop", type=float, default=10.0)
    parser.add_argument("--hold-s", type=float, default=120.0)
    parser.add_argument("--timeout-s", type=float, default=2400.0)
    parser.add_argument("--json-out")
    args = parser.parse_args()

    cam = AndorEMCCDClient(args.host, name="cool-down", acquire=True)
    t0 = time.time()
    start = cam.get_status()["temperature_c"]
    reply = cam.set_settings(cooler_on=True, target_temperature_c=args.target,
                             fan_mode=args.fan)
    print(f"start {start:.2f} C, target {args.target} C, fan {args.fan}; "
          f"warnings {reply.get('warnings', [])}", flush=True)
    ceiling = start + 3.0
    if start < 0.0:
        ceiling = max(ceiling, args.target + 15.0)
    cooling = args.target < start - args.min_drop
    log = []
    stable_since = None
    code = 0
    reason = "timeout"
    try:
        while True:
            time.sleep(args.interval_s)
            s = cam.get_status()
            t = time.time() - t0
            temp = s["temperature_c"]
            state = s["temperature_status"]
            log.append({"t_s": round(t, 1), "temperature_c": temp,
                        "status": state, "cooler_on": s["cooler_on"]})
            print(f"{t:7.0f} s  {temp:7.2f} C  {state}", flush=True)
            if temp > ceiling:
                reason, code = f"warmer than {ceiling:.1f} C", 1
                break
            if (cooling and t > args.check_after_s
                    and temp > start - args.min_drop):
                reason, code = (f"cooled less than {args.min_drop} C in "
                                f"{args.check_after_s:.0f} s"), 1
                break
            if state == "stabilized":
                stable_since = stable_since or t
                if t - stable_since >= args.hold_s:
                    reason = "stabilized"
                    break
            else:
                stable_since = None
            if t > args.timeout_s:
                break
    except KeyboardInterrupt:
        reason, code = "interrupted", 1
    if code:
        cam.set_settings(cooler_on=False)
        print(f"ABORT: {reason}; cooler switched off", flush=True)
    else:
        print(f"done: {reason}", flush=True)
    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump({"target_c": args.target, "fan": args.fan,
                       "start_c": start, "result": reason, "log": log}, f,
                      indent=1)
    cam.release_control()
    return code


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Latency / throughput benchmark of the Andor EMCCD backend service.

Measures, through the Python client:
  1. control round trip (ping, get_status),
  2. raw transport throughput (bench_payload, zero copy on the backend),
  3. fetch of a stored kinetic run (whole run and single frames),
  4. live PUB/SUB stream (full frame and a 128x128 isolated crop):
     received fps, MB/s, dropped frames and frame delivery latency.

Delivery latency is receive_time - header["timestamp"] corrected by the
client/server clock offset estimated from ping's server_time (minimum-RTT
sample, offset = server_time - (t_send + t_recv) / 2). The backend settings
are saved before and restored after the benchmark.

Examples::

    python3 bench_backend.py --ipc --ipc-dir /run/user/1000/aec
    python3 bench_backend.py --host camera-pc --json-out results/x.json
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import platform
import socket
import sys
import time
from typing import Any, Callable, Dict, List, Optional, Sequence

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from andor_emccd import (AndorEMCCDClient, AndorEMCCDError,  # noqa: E402
                         ControlDeniedError,
                         FrameSubscriber, DEFAULT_CONTROL_PORT,
                         DEFAULT_DATA_PORT)
from andor_emccd.client import DEFAULT_IPC_DIR  # noqa: E402

KIB = 1024
MIB = 1024 * 1024
FULL_FRAME = {"hbin": 1, "vbin": 1, "hstart": 1, "hend": 0, "vstart": 1,
              "vend": 0, "isolated_crop": False}
CROP_128 = {"hbin": 1, "vbin": 1, "isolated_crop": True, "crop_width": 128,
            "crop_height": 128}


class Clock:
    """Wall clock with perf_counter resolution.

    time.time() can have ~16 ms resolution on Windows before Python 3.13; a
    constant anchor error cancels between the offset estimate and the
    latency measurement because both use this clock.
    """

    def __init__(self) -> None:
        self._wall0 = time.time()
        self._pc0 = time.perf_counter()

    def now(self) -> float:
        return self._wall0 + (time.perf_counter() - self._pc0)


def stats(values: Sequence[float], scale: float = 1.0) -> Dict[str, float]:
    """min/median/p90/p99/max/mean of values * scale."""
    a = np.asarray(values, dtype=float) * scale
    if a.size == 0:
        return {}
    return {"n": int(a.size), "min": float(a.min()),
            "median": float(np.median(a)),
            "p90": float(np.percentile(a, 90)),
            "p99": float(np.percentile(a, 99)), "max": float(a.max()),
            "mean": float(a.mean())}


def time_calls(func: Callable[[], Any], n: int, warmup: int = 10
               ) -> List[float]:
    for _ in range(min(warmup, n)):
        func()
    out = []
    for _ in range(n):
        t0 = time.perf_counter()
        func()
        out.append(time.perf_counter() - t0)
    return out


def estimate_offset(client: AndorEMCCDClient, clock: Clock, n: int = 200
                    ) -> Dict[str, float]:
    """Server minus client clock, from the minimum-RTT ping."""
    best = None
    for _ in range(n):
        t0 = clock.now()
        server = client.ping()["server_time"]
        t1 = clock.now()
        rtt = t1 - t0
        if best is None or rtt < best[0]:
            best = (rtt, server - 0.5 * (t0 + t1))
    return {"offset_s": best[1], "uncertainty_s": 0.5 * best[0],
            "min_rtt_s": best[0]}


# ----- benchmark sections -----

def bench_control(client: AndorEMCCDClient, n: int) -> Dict[str, Any]:
    return {"ping_us": stats(time_calls(client.ping, n), 1e6),
            "get_status_us": stats(time_calls(client.get_status, n), 1e6)}


def bench_payload(client: AndorEMCCDClient, sizes: Sequence[int],
                  max_bytes: int) -> List[Dict[str, Any]]:
    results = []
    for size in sizes:
        reps = int(min(500, max(10, max_bytes // size)))

        def request(size: int = size) -> None:
            reply, blobs = client.request_raw("bench_payload",
                                              {"bytes": size})
            if not reply.get("ok") or len(blobs[0]) != size:
                raise AndorEMCCDError(f"bench_payload failed: {reply}")

        times = time_calls(request, reps, warmup=3)
        total = sum(times)
        results.append({
            "bytes": size, "reps": reps,
            "mb_per_s": size * reps / total / 1e6,
            "gbit_per_s": size * reps * 8 / total / 1e9,
            "latency_ms": stats(times, 1e3)})
    return results


def bench_fetch(client: AndorEMCCDClient, n_frames: int, reps: int,
                single_reps: int) -> Dict[str, Any]:
    settings = dict(FULL_FRAME, trigger_mode="internal",
                    exposure_time_s=0.001, kinetic_cycle_time_s=0.0)
    run = client.arm(n_frames, settings)
    run_id = run["run_id"]
    client.wait(run_id, timeout_s=60.0 + n_frames * 0.1)

    def raw() -> int:
        reply, blobs = client.request_raw(
            "fetch", {"run_id": run_id, "start": 0, "count": -1},
            timeout_s=60.0)
        if not reply.get("ok"):
            raise AndorEMCCDError(reply.get("error"))
        return sum(len(b) for b in blobs)

    n_bytes = raw()
    raw_times = time_calls(raw, reps, warmup=1)
    images, _ = client.fetch(run_id)
    client_times = time_calls(lambda: client.fetch(run_id), reps, warmup=1)
    index = iter(range(10 ** 9))
    single = time_calls(
        lambda: client.fetch(run_id, start=next(index) % n_frames, count=1),
        single_reps)
    best_raw, best_client = min(raw_times), min(client_times)
    return {
        "run_id": run_id, "frames": int(images.shape[0]),
        "frame_shape": list(images.shape[1:]), "bytes": n_bytes,
        "raw_s": stats(raw_times), "client_s": stats(client_times),
        "raw_mb_per_s": n_bytes / np.median(raw_times) / 1e6,
        "raw_best_mb_per_s": n_bytes / best_raw / 1e6,
        "client_mb_per_s": n_bytes / np.median(client_times) / 1e6,
        "client_frames_per_s": images.shape[0] / np.median(client_times),
        "single_frame_ms": stats(single, 1e3)}


def bench_stream(client: AndorEMCCDClient, sub_kwargs: Dict[str, Any],
                 settings: Dict[str, Any], duration_s: float, clock: Clock,
                 offset_s: float) -> Dict[str, Any]:
    sub = FrameSubscriber(frames=True, status=False, **sub_kwargs)
    try:
        time.sleep(0.3)  # let the SUB connect before frames are published
        client.stop()
        run = client.start(dict(settings, acquisition_mode="continuous",
                                trigger_mode="internal"))
        run_id = run["run_id"]
        recv_t, seqs, stamps, sizes, shape = [], [], [], [], None
        t_end = time.perf_counter() + duration_s
        while time.perf_counter() < t_end:
            msg = sub.recv(timeout_s=0.5)
            if msg is None:
                continue
            _, header, image = msg
            t = clock.now()
            if header["run_id"] != run_id:
                continue
            recv_t.append(t)
            seqs.append(header["seq"])
            stamps.append(header["timestamp"])
            sizes.append(image.nbytes)
            shape = list(image.shape)
        status = client.get_status()
        client.stop()
    finally:
        sub.close()
    result: Dict[str, Any] = {"run_id": run_id, "shape": shape,
                              "received": len(seqs),
                              "timings": run.get("timings"),
                              "backend_fps": status.get("fps")}
    if len(seqs) < 2:
        return result
    span = recv_t[-1] - recv_t[0]
    gaps = np.diff(np.asarray(seqs, dtype=np.int64))
    expected = seqs[-1] - seqs[0] + 1
    latency = (np.asarray(recv_t) - np.asarray(stamps)) + offset_s
    result.update({
        "fps": (len(seqs) - 1) / span,
        "mb_per_s": sum(sizes[1:]) / span / 1e6,
        "expected_in_span": int(expected),
        "dropped": int(expected - len(seqs)),
        "out_of_order": int((gaps <= 0).sum()),
        "latency_ms": stats(latency, 1e3)})
    return result


# ----- report -----

def _fmt_stats(s: Dict[str, float], unit: str, digits: int = 1) -> str:
    if not s:
        return "n/a"
    f = f"{{:.{digits}f}}"
    return (f"min {f.format(s['min'])}  med {f.format(s['median'])}  "
            f"p90 {f.format(s['p90'])}  p99 {f.format(s['p99'])}  "
            f"max {f.format(s['max'])} {unit}")


def _size_name(n: int) -> str:
    return f"{n // MIB} MiB" if n >= MIB else f"{n // KIB} KiB"


def report(r: Dict[str, Any]) -> str:
    lines = [f"Andor EMCCD backend benchmark  {r['time']}",
             f"client {r['client_host']} ({r['platform']}, Python "
             f"{r['python']}) -> {r['control_endpoint']}"]
    off = r["clock_offset"]
    lines.append(f"clock offset server-client {off['offset_s'] * 1e3:+.3f} ms"
                 f" (+-{off['uncertainty_s'] * 1e3:.3f} ms)"
                 + (f", drift over run {r['clock_drift_ms']:+.3f} ms"
                    if "clock_drift_ms" in r else ""))
    if "control" in r:
        c = r["control"]
        lines += ["", "1. Control round trip (us)",
                  f"   ping        {_fmt_stats(c['ping_us'], 'us')}",
                  f"   get_status  {_fmt_stats(c['get_status_us'], 'us')}"]
    if "payload" in r:
        lines += ["", "2. Raw transport (bench_payload)",
                  f"   {'size':>8} {'reps':>5} {'MB/s':>9} {'Gbit/s':>7}"
                  f" {'med ms':>8} {'p99 ms':>8}"]
        for p in r["payload"]:
            lines.append(
                f"   {_size_name(p['bytes']):>8} {p['reps']:>5} "
                f"{p['mb_per_s']:>9.1f} {p['gbit_per_s']:>7.2f} "
                f"{p['latency_ms']['median']:>8.3f} "
                f"{p['latency_ms']['p99']:>8.3f}")
    if "fetch" in r:
        f = r["fetch"]
        if "error" in f:
            lines += ["", f"3. Fetch: failed: {f['error']}"]
        else:
            lines += [
                "", f"3. Fetch of a stored run ({f['frames']} frames "
                f"{f['frame_shape'][1]}x{f['frame_shape'][0]}, "
                f"{f['bytes'] / 1e6:.1f} MB)",
                f"   raw transport  {f['raw_mb_per_s']:.1f} MB/s (median), "
                f"{f['raw_best_mb_per_s']:.1f} MB/s (best)",
                f"   client.fetch   {f['client_mb_per_s']:.1f} MB/s, "
                f"{f['client_frames_per_s']:.0f} frames/s "
                f"({f['client_s']['median'] * 1e3:.1f} ms per run)",
                f"   single frame   {_fmt_stats(f['single_frame_ms'], 'ms', 3)}"]
    if "stream" in r:
        lines += ["", "4. Live stream (PUB/SUB, continuous, internal trigger)"]
        for name, s in r["stream"].items():
            if "error" in s:
                lines.append(f"   {name}: failed: {s['error']}")
                continue
            if "fps" not in s:
                lines.append(f"   {name}: received {s['received']} frames")
                continue
            kin = (s.get("timings") or {}).get("kinetic_s")
            lines += [
                f"   {name} {s['shape'][1]}x{s['shape'][0]}: {s['fps']:.1f} fps"
                f" (camera cycle {1.0 / kin if kin else float('nan'):.1f} fps),"
                f" {s['mb_per_s']:.1f} MB/s, received {s['received']},"
                f" dropped {s['dropped']}",
                f"      delivery latency {_fmt_stats(s['latency_ms'], 'ms', 2)}"]
    return "\n".join(lines)


# ----- main -----

def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Benchmark the Andor EMCCD backend (latency/throughput).")
    p.add_argument("--host", default="localhost", help="backend host")
    p.add_argument("--control-port", type=int, default=DEFAULT_CONTROL_PORT)
    p.add_argument("--data-port", type=int, default=DEFAULT_DATA_PORT)
    g = p.add_mutually_exclusive_group()
    g.add_argument("--ipc", dest="ipc", action="store_const", const=True,
                   default=None, help="force IPC endpoints")
    g.add_argument("--tcp", dest="ipc", action="store_const", const=False,
                   help="force TCP even on the backend host")
    p.add_argument("--ipc-dir", default=DEFAULT_IPC_DIR,
                   help=f"backend IPC directory (default {DEFAULT_IPC_DIR})")
    p.add_argument("-n", "--iterations", type=int, default=2000,
                   help="control round trips per command (default 2000)")
    p.add_argument("--payload-sizes", default="64K,512K,4M,32M",
                   help="bench_payload sizes (K/M suffix)")
    p.add_argument("--payload-bytes", type=float, default=512,
                   help="target MiB transferred per payload size")
    p.add_argument("--fetch-frames", type=int, default=100)
    p.add_argument("--fetch-reps", type=int, default=5)
    p.add_argument("--stream-seconds", type=float, default=5.0)
    p.add_argument("--skip", default="",
                   help="comma list of sections to skip: "
                        "control,payload,fetch,stream")
    p.add_argument("--force-control", action="store_true",
                   help="take the settings control right even if another "
                        "client (e.g. a GUI) holds it")
    p.add_argument("--timeout", type=float, default=5.0,
                   help="seconds to wait for the first reply")
    p.add_argument("--label", default="", help="free text stored in JSON")
    p.add_argument("--json-out", help="write results as JSON here (a .txt "
                   "summary is written next to it)")
    return p.parse_args(argv)


def _parse_size(text: str) -> int:
    text = text.strip().upper()
    mult = {"K": KIB, "M": MIB}.get(text[-1:], 1)
    return int(float(text.rstrip("KM")) * mult)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    skip = {s.strip() for s in args.skip.split(",") if s.strip()}
    conn = dict(host=args.host, control_port=args.control_port,
                data_port=args.data_port, ipc=args.ipc, ipc_dir=args.ipc_dir)
    client = AndorEMCCDClient(timeout_s=args.timeout, name="bench", **conn)
    print(f"Connecting to {client.control_endpoint} ...", flush=True)
    try:
        hello = client.ping()
    except TimeoutError:
        print(f"error: no reply from the backend at {client.control_endpoint}"
              f" within {args.timeout:.1f} s. Is andor_emccd_backend running"
              " and are TCP ports "
              f"{args.control_port}/{args.data_port} open in the backend "
              "host's firewall?", file=sys.stderr)
        client.close()
        return 2
    client.timeout_s = max(args.timeout, 30.0)
    try:
        client.acquire_control(force=args.force_control)
    except ControlDeniedError as e:
        print(f"error: {e}. Release control in that client or use "
              "--force-control.", file=sys.stderr)
        client.close()
        return 3
    clock = Clock()
    info = client.get_info()
    original = client.get_settings()
    result: Dict[str, Any] = {
        "time": datetime.datetime.now().astimezone().isoformat(
            timespec="seconds"),
        "label": args.label, "client_host": socket.gethostname(),
        "platform": platform.platform(), "python": platform.python_version(),
        "control_endpoint": client.control_endpoint,
        "data_endpoint": client.data_endpoint,
        "simulated": hello.get("simulated"), "camera": info.get("model"),
        "args": vars(args)}
    print(f"Backend: {info.get('model')} (protocol {hello.get('protocol')})")
    try:
        result["clock_offset"] = estimate_offset(client, clock)
        if "control" not in skip:
            print("1/4 control round trip ...", flush=True)
            result["control"] = bench_control(client, args.iterations)
        if "payload" not in skip:
            print("2/4 raw transport ...", flush=True)
            sizes = [_parse_size(s) for s in args.payload_sizes.split(",")]
            result["payload"] = bench_payload(
                client, sizes, int(args.payload_bytes * MIB))
        if "fetch" not in skip:
            print("3/4 fetch ...", flush=True)
            try:
                result["fetch"] = bench_fetch(client, args.fetch_frames,
                                              args.fetch_reps, 50)
            except (AndorEMCCDError, TimeoutError) as e:
                result["fetch"] = {"error": str(e)}
            client.stop()
        if "stream" not in skip:
            print("4/4 live stream ...", flush=True)
            off = result["clock_offset"]["offset_s"]
            result["stream"] = {}
            for name, settings in (("full", FULL_FRAME),
                                   ("crop128", CROP_128)):
                try:
                    result["stream"][name] = bench_stream(
                        client, conn, dict(settings, exposure_time_s=0.001,
                                           kinetic_cycle_time_s=0.0),
                        args.stream_seconds, clock, off)
                except (AndorEMCCDError, TimeoutError) as e:
                    result["stream"][name] = {"error": str(e)}
        end = estimate_offset(client, clock, 50)
        result["clock_offset_end"] = end
        result["clock_drift_ms"] = (end["offset_s"]
                                    - result["clock_offset"]["offset_s"]) * 1e3
    except TimeoutError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130
    finally:
        try:
            client.stop()
            client.set_settings(original)
        except (AndorEMCCDError, TimeoutError) as e:
            print(f"warning: could not restore settings: {e}",
                  file=sys.stderr)
        client.close()

    text = report(result)
    print()
    print(text)
    if args.json_out:
        os.makedirs(os.path.dirname(os.path.abspath(args.json_out)),
                    exist_ok=True)
        with open(args.json_out, "w") as f:
            json.dump(result, f, indent=2)
        with open(os.path.splitext(args.json_out)[0] + ".txt", "w") as f:
            f.write(text + "\n")
        print(f"\nwrote {args.json_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

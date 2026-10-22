"""Python client for the Andor EMCCD backend service.

Talks to the backend over ZeroMQ. On the backend host the IPC (Unix socket)
endpoints are used automatically; elsewhere TCP is used.

Example::

    from andor_emccd import AndorEMCCDClient
    cam = AndorEMCCDClient("camera-pc")
    cam.set_settings(exposure_time_s=0.005, trigger_mode="external")
    run = cam.arm(n_frames=3)
    cam.wait(run["run_id"], timeout_s=10)
    images, headers = cam.fetch(run["run_id"])
"""

from __future__ import annotations

from collections import deque
import itertools
import json
import os
import socket
import threading
import time
import uuid
from typing import Any, Callable, Dict, Iterator, List, Optional, Tuple

import numpy as np
import zmq

DEFAULT_CONTROL_PORT = 47100
DEFAULT_DATA_PORT = 47101
DEFAULT_IPC_DIR = "/run/andor-emccd"

_DTYPES = {"uint16": np.dtype("<u2"), "int32": np.dtype("<i4"),
           "float32": np.dtype("<f4")}


class AndorEMCCDError(RuntimeError):
    """Error reported by the backend."""

    def __init__(self, message: str, result: Any = None,
                 code: Optional[str] = None):
        super().__init__(message)
        self.result = result
        self.code = code


class NotControllerError(AndorEMCCDError):
    """A changing command was rejected: another client holds control.

    ``result`` holds the control information ({"holder", ...} or None).
    """


class ControlDeniedError(AndorEMCCDError):
    """acquire_control was refused because another client holds control."""


_ERRORS_BY_CODE: Dict[str, type] = {}


class WaitTimeoutError(AndorEMCCDError):
    """A "wait" request ended before the run had the requested frames.

    ``result`` holds the run information (frames acquired so far).
    """


_ERRORS_BY_CODE.update({"timeout": WaitTimeoutError,
                        "not_controller": NotControllerError,
                        "control_denied": ControlDeniedError})


def local_address(host: str = "8.8.8.8") -> str:
    """IPv4 address of this machine on the route to host (no packets sent)."""
    for target in (host, "8.8.8.8"):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.connect((socket.gethostbyname(target), 9))
                addr = s.getsockname()[0]
            if not addr.startswith("127."):
                return addr
        except OSError:
            continue
    return "127.0.0.1"


def _is_local_host(host: str) -> bool:
    if host in ("localhost", "127.0.0.1", "::1", ""):
        return True
    try:
        addr = socket.gethostbyname(host)
    except OSError:
        return False
    if addr.startswith("127."):
        return True
    try:
        local = {i[4][0] for i in socket.getaddrinfo(socket.gethostname(), None)}
    except OSError:
        local = set()
    return addr in local


def resolve_endpoints(host: str = "localhost",
                      control_port: int = DEFAULT_CONTROL_PORT,
                      data_port: int = DEFAULT_DATA_PORT,
                      ipc: Optional[bool] = None,
                      ipc_dir: str = DEFAULT_IPC_DIR) -> Tuple[str, str]:
    """Returns (control, data) endpoints; IPC when on the backend host."""
    if ipc is None:
        ipc = (os.name == "posix" and _is_local_host(host)
               and os.path.exists(os.path.join(ipc_dir, "control")))
    if ipc:
        return (f"ipc://{ipc_dir}/control", f"ipc://{ipc_dir}/data")
    return (f"tcp://{host}:{control_port}", f"tcp://{host}:{data_port}")


def frame_to_array(header: Dict[str, Any], payload) -> np.ndarray:
    """Wraps a frame payload as a (height, width) array without copying."""
    dtype = _DTYPES[header.get("dtype", "uint16")]
    return np.frombuffer(payload, dtype=dtype).reshape(
        header["height"], header["width"])


class AndorEMCCDClient:
    """Request/reply client. Thread-safe (requests are serialized).

    Changing commands (set_settings, start, arm, stop, software_trigger)
    need the control right: call acquire_control() first (or pass
    acquire=True). While control is held a background thread sends a
    keepalive every 2 s; the backend frees control after ~10 s of silence.
    Other clients see the holder as "name@ip".
    """

    def __init__(self, host: str = "localhost",
                 control_port: int = DEFAULT_CONTROL_PORT,
                 data_port: int = DEFAULT_DATA_PORT,
                 ipc: Optional[bool] = None,
                 ipc_dir: str = DEFAULT_IPC_DIR,
                 timeout_s: float = 10.0,
                 context: Optional[zmq.Context] = None,
                 name: str = "",
                 acquire: bool = False,
                 force: bool = False):
        self.control_endpoint, self.data_endpoint = resolve_endpoints(
            host, control_port, data_port, ipc, ipc_dir)
        self.timeout_s = timeout_s
        self.client_id = uuid.uuid4().hex
        self.name = name
        self._host_address = local_address(
            "8.8.8.8" if _is_local_host(host) else host)
        self._keepalive: Optional[threading.Thread] = None
        self._keepalive_stop = threading.Event()
        self._context = context or zmq.Context.instance()
        self._lock = threading.Lock()
        self._ids = itertools.count(1)
        self._socket: Optional[zmq.Socket] = None
        self._connect()
        if acquire:
            self.acquire_control(force=force)

    # ----- transport -----

    def _connect(self) -> None:
        if self._socket is not None:
            self._socket.close(linger=0)
        s = self._context.socket(zmq.DEALER)
        s.setsockopt(zmq.LINGER, 0)
        s.setsockopt(zmq.RCVHWM, 0)
        s.connect(self.control_endpoint)
        self._socket = s

    def close(self) -> None:
        if self._keepalive is not None:
            try:
                self.release_control()
            except (AndorEMCCDError, TimeoutError, zmq.ZMQError):
                pass
        with self._lock:
            if self._socket is not None:
                self._socket.close(linger=0)
                self._socket = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def request_raw(self, cmd: str, args: Optional[Dict[str, Any]] = None,
                    timeout_s: Optional[float] = None
                    ) -> Tuple[Dict[str, Any], List[zmq.Frame]]:
        """Sends one request; returns (reply json, binary frames)."""
        timeout = self.timeout_s if timeout_s is None else timeout_s
        with self._lock:
            if self._socket is None:
                self._connect()
            req_id = next(self._ids)
            body = json.dumps({
                "id": req_id, "cmd": cmd, "args": args or {},
                "client": {"id": self.client_id, "name": self.name,
                           "host": self._host_address}}).encode()
            self._socket.send_multipart([b"", body])
            deadline = time.monotonic() + timeout
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not self._socket.poll(
                        int(remaining * 1000) + 1, zmq.POLLIN):
                    # Drop the socket so a late reply cannot be mistaken for
                    # the answer to the next request.
                    self._connect()
                    raise TimeoutError(
                        f"no reply to {cmd!r} from {self.control_endpoint} "
                        f"within {timeout:.1f} s")
                parts = self._socket.recv_multipart(copy=False)
                # [empty, json, binary...]
                reply = json.loads(parts[1].bytes)
                if reply.get("id") == req_id:
                    return reply, parts[2:]

    def request(self, cmd: str, args: Optional[Dict[str, Any]] = None,
                timeout_s: Optional[float] = None) -> Any:
        reply, _ = self.request_raw(cmd, args, timeout_s)
        if not reply.get("ok"):
            cls = _ERRORS_BY_CODE.get(reply.get("code"), AndorEMCCDError)
            raise cls(reply.get("error", "unknown error"),
                      reply.get("result"), reply.get("code"))
        return reply.get("result")

    # ----- control right -----

    def acquire_control(self, force: bool = False) -> Dict[str, Any]:
        """Requests the right to change settings.

        Raises ControlDeniedError if another client holds it (force=True
        takes it over). Returns {"holder", "holder_id", "since"}.
        """
        control = self.request("acquire_control", {"force": bool(force)})
        self._start_keepalive()
        return control

    def release_control(self) -> None:
        self._stop_keepalive()
        self.request("release_control")

    def get_control(self) -> Optional[Dict[str, Any]]:
        """Current holder, or None if nobody holds control."""
        return self.request("get_control")

    def has_control(self) -> bool:
        control = self.get_control()
        return bool(control) and control.get("holder_id") == self.client_id

    def _start_keepalive(self) -> None:
        if self._keepalive is not None and self._keepalive.is_alive():
            return
        self._keepalive_stop.clear()
        # Own socket, so a long wait() on this client cannot delay it.
        keeper = AndorEMCCDClient.__new__(AndorEMCCDClient)
        keeper.control_endpoint = self.control_endpoint
        keeper.timeout_s = 2.0
        keeper.client_id = self.client_id
        keeper.name = self.name
        keeper._host_address = self._host_address
        keeper._context = self._context
        keeper._lock = threading.Lock()
        keeper._ids = itertools.count(1)
        keeper._socket = None
        keeper._keepalive = None

        def run() -> None:
            while not self._keepalive_stop.wait(2.0):
                try:
                    keeper.request("ping")
                except (AndorEMCCDError, TimeoutError, zmq.ZMQError):
                    pass
            if keeper._socket is not None:
                keeper._socket.close(linger=0)

        self._keepalive = threading.Thread(target=run, daemon=True,
                                           name="andor-emccd-keepalive")
        self._keepalive.start()

    def _stop_keepalive(self) -> None:
        if self._keepalive is not None:
            self._keepalive_stop.set()
            self._keepalive.join(timeout=3.0)
            self._keepalive = None

    # ----- commands -----

    def ping(self) -> Dict[str, Any]:
        return self.request("ping")

    def get_info(self) -> Dict[str, Any]:
        return self.request("get_info")

    def get_settings(self) -> Dict[str, Any]:
        return self.request("get_settings")

    def set_settings(self, settings: Optional[Dict[str, Any]] = None,
                     **kwargs) -> Dict[str, Any]:
        """Updates the given keys; returns {"settings", "warnings", ...}."""
        patch = dict(settings or {})
        patch.update(kwargs)
        return self.request("set_settings", {"settings": patch})

    def get_status(self) -> Dict[str, Any]:
        return self.request("get_status")

    def start(self, settings: Optional[Dict[str, Any]] = None
              ) -> Dict[str, Any]:
        args = {"settings": settings} if settings else {}
        return self.request("start", args)

    def stop(self) -> Dict[str, Any]:
        return self.request("stop")

    def arm(self, n_frames: int, settings: Optional[Dict[str, Any]] = None,
            **kwargs) -> Dict[str, Any]:
        """Starts a kinetic series of n_frames (usually external trigger)."""
        patch = dict(settings or {})
        patch.update(kwargs)
        return self.request("arm", {"n_frames": int(n_frames),
                                    "settings": patch})

    def software_trigger(self) -> Dict[str, Any]:
        return self.request("software_trigger")

    def get_runs(self) -> List[Dict[str, Any]]:
        return self.request("get_runs")

    def wait(self, run_id: Optional[int] = None,
             n_frames: Optional[int] = None,
             timeout_s: float = 10.0) -> Dict[str, Any]:
        """Blocks until the run has n_frames frames (default: all).

        Raises WaitTimeoutError if they do not arrive within timeout_s. The
        client is locked while waiting; use a second client to send other
        requests (e.g. stop) meanwhile.
        """
        args: Dict[str, Any] = {"timeout_s": timeout_s}
        if run_id is not None:
            args["run_id"] = int(run_id)
        if n_frames is not None:
            args["n_frames"] = int(n_frames)
        return self.request("wait", args, timeout_s=timeout_s + 5.0)

    def fetch(self, run_id: Optional[int] = None, start: int = 0,
              count: int = -1, stack: bool = True
              ) -> Tuple[Any, List[Dict[str, Any]]]:
        """Returns (images, headers) of a run (default: the latest run).

        With stack=True images is one (n, h, w) array (a copy). With
        stack=False it is a list of zero-copy views of the
        received buffers, which is about twice as fast for large runs.
        """
        args: Dict[str, Any] = {"start": int(start), "count": int(count)}
        if run_id is not None:
            args["run_id"] = int(run_id)
        reply, blobs = self.request_raw("fetch", args,
                                        timeout_s=max(self.timeout_s, 30.0))
        if not reply.get("ok"):
            raise AndorEMCCDError(reply.get("error", "fetch failed"))
        headers = reply["result"]["frames"]
        arrays = [frame_to_array(h, b.buffer) for h, b in zip(headers, blobs)]
        if not stack:
            return arrays, headers
        if not arrays:
            return np.empty((0, 0, 0), dtype=np.uint16), headers
        same = all(a.shape == arrays[0].shape and a.dtype == arrays[0].dtype
                   for a in arrays)
        if same:
            return np.stack(arrays), headers
        return [a.copy() for a in arrays], headers

    def fetch_latest(self, after_seq: Optional[int] = None,
                     run_id: Optional[int] = None
                     ) -> Tuple[Optional[np.ndarray], Optional[Dict[str, Any]]]:
        """Most recent frame, optionally only if newer than after_seq and
        from the given run. Returns (None, None) if there is none."""
        args: Dict[str, Any] = {}
        if after_seq is not None:
            args["after_seq"] = int(after_seq)
        if run_id is not None:
            args["run_id"] = int(run_id)
        reply, blobs = self.request_raw("fetch_latest", args)
        if not reply.get("ok"):
            raise AndorEMCCDError(reply.get("error", "fetch failed"))
        headers = reply["result"]["frames"]
        if not headers:
            return None, None
        return frame_to_array(headers[0], blobs[0].buffer).copy(), headers[0]

    def open_stream(self) -> Dict[str, Any]:
        """Register a send cursor at the next acquired frame (60 s lease)."""
        result = self.request("stream_open")
        self._stream_ack = 0
        return result

    def read_stream(self, count: int = 8):
        """Return (images, headers, delivery). Advances our server send cursor.

        delivery['loss'] always exists, including count=0. Loss timestamps
        are acquisition Unix timestamps of the first and last evicted frame.
        The next read acknowledges this batch. Retrying a timed-out read
        replays the unacknowledged batch without advancing the send cursor.
        """
        reply, blobs = self.request_raw("stream_read", {"count": count,
                                                          "ack": self._stream_ack})
        if not reply.get("ok"):
            raise AndorEMCCDError(reply.get("error", "stream read failed"))
        result = reply["result"]
        headers = result["frames"]
        if len(headers) != len(blobs):
            raise AndorEMCCDError("stream payload count mismatch")
        arrays = [frame_to_array(h, b.buffer) for h, b in zip(headers, blobs)]
        self._stream_ack = result["batch_id"]
        return arrays, headers, result

    def close_stream(self) -> None:
        self.request("stream_close")

    def acquire(self, n_frames: int = 1, timeout_s: float = 30.0,
                settings: Optional[Dict[str, Any]] = None
                ) -> Tuple[np.ndarray, List[Dict[str, Any]]]:
        """Arm, wait and fetch in one call."""
        run = self.arm(n_frames, settings)
        self.wait(run["run_id"], timeout_s=timeout_s)
        return self.fetch(run["run_id"])


def _merge_loss(target, loss):
    count = loss.get("count", 0)
    if not count:
        return
    previous = target.get("count", 0)
    if not previous:
        target.update(loss)
    else:
        target["last_seq"] = loss["last_seq"]
        target["last_timestamp"] = loss["last_timestamp"]
    target["count"] = previous + count


class FrameSubscriber:
    """Background receiver with a bounded frame ring (oldest overwritten).

    hwm specifies the ring length in frames. recv() frame metadata contains
    delivery.server_loss and delivery.client_loss, each with count (also 0),
    sequence bounds and acquisition timestamp bounds when nonzero.
    The cursor starts after stream_open; older history is available via fetch.
    Status uses a separate latest-value slot. Call close() to join the worker.
    """

    def __init__(self, host: str = "localhost",
                 control_port: int = DEFAULT_CONTROL_PORT,
                 data_port: int = DEFAULT_DATA_PORT,
                 ipc: Optional[bool] = None,
                 ipc_dir: str = DEFAULT_IPC_DIR,
                 frames: bool = True, status: bool = True,
                 hwm: int = 64,
                 context: Optional[zmq.Context] = None,
                 buffer_frames: Optional[int] = None):
        capacity = hwm if buffer_frames is None else buffer_frames
        if not isinstance(capacity, int) or capacity < 1:
            raise ValueError("buffer_frames must be a positive integer")
        self._capacity = capacity
        self._queue = deque()
        self._condition = threading.Condition()
        self._stop = threading.Event()
        self._ready = threading.Event()
        self._error = None
        self._status = None
        self._server_loss = {"count": 0}
        self._client_loss = {"count": 0}
        self.dropped = 0
        _, self.endpoint = resolve_endpoints(host, control_port, data_port,
                                             ipc, ipc_dir)

        def receive():
            client = None
            try:
                client = AndorEMCCDClient(host, control_port, data_port, ipc,
                                         ipc_dir, timeout_s=2, context=context)
                if frames:
                    client.open_stream()
                self._ready.set()
                next_status = 0
                while not self._stop.is_set():
                    arrays, headers, result = ([], [], {"loss": {"count": 0}})
                    if frames:
                        arrays, headers, result = client.read_stream(
                            min(8, capacity))
                    with self._condition:
                        _merge_loss(self._server_loss, result["loss"])
                        self.dropped += result["loss"]["count"]
                        for header, image in zip(headers, arrays):
                            if len(self._queue) >= capacity:
                                old, _ = self._queue.popleft()
                                _merge_loss(self._client_loss, {
                                    "count": 1, "first_seq": old["seq"],
                                    "last_seq": old["seq"],
                                    "first_timestamp": old["timestamp"],
                                    "last_timestamp": old["timestamp"]})
                                self.dropped += 1
                            self._queue.append((header, image))
                        self._condition.notify_all()
                    if status and time.monotonic() >= next_status:
                        current = client.get_status()
                        with self._condition:
                            self._status = current
                            self._condition.notify_all()
                        next_status = time.monotonic() + 0.5
                    if not arrays:
                        self._stop.wait(0.01)
            except Exception as error:
                with self._condition:
                    self._error = error
                    self._condition.notify_all()
            finally:
                self._ready.set()
                if client is not None:
                    try:
                        if frames:
                            client.close_stream()
                    except Exception:
                        pass
                    client.close()

        self._thread = threading.Thread(target=receive, daemon=True,
                                        name="andor-frame-ring")
        self._thread.start()
        if not self._ready.wait(5):
            self.close()
            raise TimeoutError("stream registration timed out")
        if self._error:
            self.close()
            raise self._error

    def close(self) -> None:
        self._stop.set()
        with self._condition:
            self._condition.notify_all()
        self._thread.join()

    def recv(self, timeout_s: Optional[float] = None, copy: bool = False):
        """Return (topic, metadata, array), or None on timeout/close."""
        deadline = None if timeout_s is None else time.monotonic() + timeout_s
        with self._condition:
            while True:
                if self._queue:
                    header, image = self._queue.popleft()
                    header = dict(header)
                    header["delivery"] = {
                        "server_loss": self._server_loss,
                        "client_loss": self._client_loss,
                        "reported_at": time.time()}
                    self._server_loss = {"count": 0}
                    self._client_loss = {"count": 0}
                    return "frame", header, image.copy() if copy else image
                if self._status is not None:
                    current, self._status = self._status, None
                    return "status", current, None
                if self._error:
                    raise self._error
                if self._stop.is_set():
                    return None
                remaining = None if deadline is None else deadline - time.monotonic()
                if remaining is not None and remaining <= 0:
                    return None
                self._condition.wait(remaining)

    def __iter__(self) -> Iterator:
        while not self._stop.is_set():
            item = self.recv()
            if item is not None:
                yield item

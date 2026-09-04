"""Small, deliberately serial JSON transport shared with Max for Live."""

from __future__ import annotations

import json
import errno
import os
import re
import socket
import tempfile
import threading
import time
import uuid
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator


class BridgeError(RuntimeError):
    """The bridge could not complete a command safely."""


class BridgeTimeout(BridgeError):
    pass


def local_path(value: str | Path) -> Path:
    """Accept Windows paths from manifests when the runner is in WSL."""
    raw = os.fspath(value)
    match = re.match(r"^([A-Za-z]):[\\/](.*)$", raw)
    if os.name != "nt" and match:
        return Path("/mnt", match[1].lower(), match[2].replace("\\", "/"))
    return Path(raw).expanduser()


def host_path(path: str | Path, platform: str = "") -> str:
    """Use paths Max on Windows can open, even when Python runs in WSL."""
    resolved = local_path(path).resolve()
    match = re.match(r"^/mnt/([A-Za-z])/(.*)$", resolved.as_posix())
    if match:
        return f"{match[1].upper()}:/{match[2]}"
    if "win" in platform.lower() and os.name != "nt":
        raise ValueError(
            f"Windows Live cannot open {resolved}; put the bridge and run files "
            "on a Windows drive, for example /mnt/c/Users/.../LiveProbe"
        )
    return resolved.as_posix()


def atomic_json(path: Path, value: Any) -> None:
    """Publish complete JSON with an atomic rename on the same filesystem."""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        # Max may have the old file open momentarily on Windows.
        deadline = time.monotonic() + 2.0
        while True:
            try:
                os.replace(temporary, path)
                break
            except PermissionError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.025)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def read_json(path: Path) -> Any:
    with path.open(encoding="utf-8-sig") as stream:
        return json.load(stream)


def try_read_json(path: Path) -> Any | None:
    try:
        return read_json(path)
    except (FileNotFoundError, PermissionError, json.JSONDecodeError):
        return None
    except OSError as exc:
        # Max truncates/replaces its response while WSL reads over DrvFS.
        # That race can surface as ENODATA instead of a short/invalid JSON
        # read. Treat it as an incomplete publication; request() owns the
        # bounded retry deadline. Other filesystem failures stay visible.
        if exc.errno == getattr(errno, "ENODATA", 61):
            return None
        raise


@contextmanager
def session_lock(root: Path) -> Iterator[None]:
    """Prevent two Python clients from overwriting each other's commands."""
    root.mkdir(parents=True, exist_ok=True)
    path = root / "client.lock"
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError as exc:
        raise BridgeError(
            f"Another client owns {path}. If that client has exited, remove "
            "client.lock after checking no batch is running."
        ) from exc
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump({"pid": os.getpid(), "host": socket.gethostname(),
                       "started": time.time()}, stream)
        yield
    finally:
        path.unlink(missing_ok=True)


class Endpoint:
    def __init__(self, root: Path, role: str, timeout: float = 10.0,
                 poll_interval: float = 0.025):
        self.directory = root / role
        self.role = role
        self.timeout = timeout
        self.poll_interval = poll_interval
        self._lock = threading.Lock()

    def request(self, action: str, args: dict[str, Any] | None = None,
                *, timeout: float | None = None) -> dict[str, Any]:
        with self._lock:
            return self._request(action, args or {}, timeout=timeout)

    def _request(self, action: str, args: dict[str, Any],
                 *, timeout: float | None) -> dict[str, Any]:
        command_path = self.directory / "command.json"
        response_path = self.directory / "response.json"
        # A timed-out command may still be executing. Never replace it with a
        # second command; that could start two recordings or lose a stop.
        previous = try_read_json(command_path)
        response = try_read_json(response_path)
        if previous is not None:
            if not isinstance(previous, dict) or not previous.get("id"):
                raise BridgeError(f"Invalid existing command in {command_path}")
            if not isinstance(response, dict) or response.get("id") != previous["id"]:
                raise BridgeError(
                    f"{self.role} has an unanswered command ({previous.get('action')}, "
                    f"id={previous['id']}). Open Live and let it finish; do not run "
                    "another batch until the bridge responds."
                )
        elif command_path.exists():
            raise BridgeError(f"Unreadable existing command in {command_path}")
        command_id = uuid.uuid4().hex
        atomic_json(command_path, {"id": command_id, "action": action, "args": args})
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while True:
            response = try_read_json(response_path)
            if isinstance(response, dict) and response.get("id") == command_id:
                if not isinstance(response.get("ok"), bool):
                    raise BridgeError(f"Malformed {self.role} response: missing boolean ok")
                if not response["ok"]:
                    error = response.get("error", "unknown error")
                    raise BridgeError(f"{self.role}.{action}: {error}")
                result = response.get("result", {})
                if not isinstance(result, dict):
                    raise BridgeError(f"Malformed {self.role} response: result is not an object")
                return result
            if time.monotonic() >= deadline:
                raise BridgeTimeout(
                    f"Timed out waiting for {self.role}.{action} (id={command_id}). "
                    f"Check Live, the Max console, and {response_path}. The pending "
                    "command was kept so it cannot be overwritten."
                )
            time.sleep(self.poll_interval)

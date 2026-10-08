import atexit
import contextlib
import errno
import os
import random
import socket
import tomllib
from pathlib import Path
from typing import Any


def _bindable(port: int) -> bool:
    # postgres binds both stacks on localhost, so both must be free.
    # a family missing on the host is fine, postgres skips it too.
    for family, addr in ((socket.AF_INET, "127.0.0.1"), (socket.AF_INET6, "::1")):
        try:
            with socket.socket(family) as s:
                s.bind((addr, port))
        except OSError as e:
            if e.errno in (errno.EADDRINUSE, errno.EACCES):
                return False
    return True


# ports are reserved host-wide with lock files, like PostgreSQL::Test::Cluster's
# get_free_port: a bind probe alone races with other sessions, and a server that
# is stopped and restarted later must keep its port meanwhile
PORT_LOCK_DIR = Path(
    os.environ.get("STORMWEAVER_PORT_LOCK_DIR", "/tmp/stormweaver-ports")
)
_held_locks: list[Path] = []


def _release_ports() -> None:
    for lock in _held_locks:
        lock.unlink(missing_ok=True)
    _held_locks.clear()


atexit.register(_release_ports)


def _pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _lock_dir() -> Path:
    # shared between users on one host, like /tmp itself
    if not PORT_LOCK_DIR.exists():
        PORT_LOCK_DIR.mkdir(parents=True, exist_ok=True)
        with contextlib.suppress(OSError):
            PORT_LOCK_DIR.chmod(0o1777)
    return PORT_LOCK_DIR


def _reserve(port: int) -> Path | None:
    lock = _lock_dir() / str(port)
    try:
        fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
    except PermissionError:
        return None
    except FileExistsError:
        try:
            owner = int(lock.read_text().strip() or 0)
        except OSError, ValueError:
            return None
        if owner and _pid_alive(owner):
            return None
        # stale lock from a dead process: take it over
        lock.unlink(missing_ok=True)
        return _reserve(port)
    with os.fdopen(fd, "w") as f:
        f.write(str(os.getpid()))
    return lock


def alloc_port(low: int = 26600, high: int = 28000) -> int:
    """Random free port, reserved for this process until exit."""
    for _ in range(400):
        port = random.randrange(low, high)
        lock = _reserve(port)
        if lock is None:
            continue
        if _bindable(port):
            _held_locks.append(lock)
            return port
        lock.unlink(missing_ok=True)
    raise RuntimeError(f"no free port in range {low}-{high}")


DEFAULT_CONFIG_PATHS = (
    Path("config/stormweaver.toml"),
    Path("/etc/stormweaver/stormweaver.toml"),
)


def resolve_config_path(explicit: str | Path | None) -> Path | None:
    """-c, then STORMWEAVER_CONFIG, then the first existing default; None if nothing."""
    if explicit:
        return Path(explicit)
    env = os.environ.get("STORMWEAVER_CONFIG")
    if env:
        return Path(env)
    for candidate in DEFAULT_CONFIG_PATHS:
        if candidate.is_file():
            return candidate
    return None


class Config:
    def __init__(self, data: dict[str, Any]) -> None:
        self._data = data
        defaults = data.get("default", {})
        self.pgroot: str = defaults.get("pgroot", "")
        self.datadir_root: str = defaults.get("datadir_root", "datadirs")
        self.port_start: int = defaults.get("port_start", 15432)
        self.port_end: int = defaults.get("port_end", 15531)
        self._allocated_ports: set[int] = set()
        self.keyrings: dict[str, Any] = data.get("keyring", {})

    @classmethod
    def load(cls, path: str | Path | None) -> Config:
        resolved = resolve_config_path(path)
        if resolved is None:
            return cls({})
        with open(resolved, "rb") as f:
            return cls(tomllib.load(f))

    def datadir(self, name: str) -> str:
        return str(Path(self.datadir_root) / f"datadir_{name}")

    def free_port(self) -> int:
        # random pick avoids collisions with other stormweaver runs on the box
        candidates = [
            p
            for p in range(self.port_start, self.port_end + 1)
            if p not in self._allocated_ports
        ]
        random.shuffle(candidates)
        for port in candidates:
            if _bindable(port):
                self._allocated_ports.add(port)
                return port
        raise RuntimeError(f"no free port in range {self.port_start}-{self.port_end}")

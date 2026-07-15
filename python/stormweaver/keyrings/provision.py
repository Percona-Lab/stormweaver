import functools
import itertools
import logging
import os
import shlex
import shutil
import socket
import ssl
import subprocess
import time
import urllib.parse
import urllib.request
from collections.abc import Callable, Iterable, Mapping, Sequence
from pathlib import Path

from stormweaver.keyrings.base import RunningService

logger = logging.getLogger(__name__)

Ready = Callable[[], bool]

_name_seq = itertools.count(1)


def container_name(kind: str) -> str:
    # pid+seq: parallel runs on one box must not collide
    return f"sw-kr-{kind}-{os.getpid()}-{next(_name_seq)}"


def file_ready(path: Path) -> Ready:
    def probe() -> bool:
        return path.exists() and path.stat().st_size > 0

    return probe


def http_ready(url: str) -> Ready:
    # test services use throwaway self-signed certs
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    def probe() -> bool:
        try:
            with urllib.request.urlopen(url, timeout=1, context=ctx):
                return True
        except Exception:
            return False

    return probe


def check_reachable(url: str, timeout: float = 5.0) -> None:
    parsed = urllib.parse.urlparse(url if "//" in url else f"//{url}")
    host, port = parsed.hostname, parsed.port
    if host is None or port is None:
        raise RuntimeError(f"cannot parse host/port from {url!r}")
    try:
        with socket.create_connection((host, port), timeout=timeout):
            pass
    except OSError as e:
        raise RuntimeError(f"keyring endpoint {host}:{port} not reachable: {e}") from e


def wait_ready(
    ready: Ready, what: str, diag: Callable[[], str], timeout: float = 30.0
) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if ready():
            return
        time.sleep(0.2)
    raise RuntimeError(f"{what} not ready after {timeout}s\n{diag()}")


@functools.lru_cache
def detect_runtime(preferred: str | None = None) -> str | None:
    for rt in [preferred] if preferred else ["podman", "docker"]:
        if shutil.which(rt) is None:
            continue
        cp = subprocess.run([rt, "info"], capture_output=True, text=True)
        if cp.returncode == 0:
            return rt
    return None


def _wipe(paths: Iterable[Path]) -> None:
    for p in paths:
        if p.is_dir():
            shutil.rmtree(p, ignore_errors=True)
        else:
            p.unlink(missing_ok=True)


class ProcessService(RunningService):
    """Locally spawned service; output collected in log_path."""

    def __init__(
        self,
        argv: Sequence[str | Path],
        *,
        what: str,
        log_path: Path,
        ready: Ready,
        env: Mapping[str, str] | None = None,
        pre_start: Iterable[Path] = (),
        fresh_paths: Iterable[Path] = (),
        on_started: Callable[[], None] | None = None,
        ready_timeout: float = 30.0,
    ) -> None:
        self.argv = [str(a) for a in argv]
        self.what = what
        self.log_path = log_path
        self.ready = ready
        self.env = dict(env) if env else None
        self.pre_start = list(pre_start)
        self.fresh_paths = list(fresh_paths)
        self.on_started = on_started
        self.ready_timeout = ready_timeout
        self._proc: subprocess.Popen[bytes] | None = None

    def _diag(self) -> str:
        if not self.log_path.exists():
            return "(no log)"
        return "log tail:\n" + self.log_path.read_text(errors="replace")[-2000:]

    def start(self) -> None:
        _wipe(self.pre_start)
        logger.info("starting %s: %s", self.what, shlex.join(self.argv))
        env = (dict(os.environ) | self.env) if self.env else None
        with open(self.log_path, "ab") as log:
            self._proc = subprocess.Popen(
                self.argv,
                stdout=log,
                stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL,
                env=env,
            )
        try:
            wait_ready(self.ready, self.what, self._diag, self.ready_timeout)
            if self.on_started is not None:
                self.on_started()
        except BaseException:
            self.stop()
            raise

    def stop(self) -> None:
        proc, self._proc = self._proc, None
        if proc is None or proc.poll() is not None:
            return
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)

    def wipe_state(self) -> None:
        _wipe(self.fresh_paths)


class ContainerService(RunningService):
    """Detached container; state that must survive restarts lives in mounts."""

    def __init__(
        self,
        runtime: str,
        *,
        name: str,
        image: str,
        what: str,
        ready: Ready,
        cmd: Sequence[str] = (),
        env: Mapping[str, str] | None = None,
        mounts: Sequence[tuple[Path, str]] = (),
        world_readable: Sequence[str] = (),  # container paths to a+r after ready
        ports: Mapping[int, int] | None = None,  # host -> container
        user: str | None = None,
        pre_start: Iterable[Path] = (),
        fresh_paths: Iterable[Path] = (),
        on_started: Callable[[], None] | None = None,
        ready_timeout: float = 60.0,
    ) -> None:
        self.runtime = runtime
        self.name = name
        self.image = image
        self.what = what
        self.ready = ready
        self.cmd = list(cmd)
        self.env = dict(env) if env else {}
        self.mounts = list(mounts)
        self.world_readable = list(world_readable)
        self.ports = dict(ports) if ports else {}
        self.user = user
        self.pre_start = list(pre_start)
        self.fresh_paths = list(fresh_paths)
        self.on_started = on_started
        self.ready_timeout = ready_timeout

    def run_args(self) -> list[str]:
        args = ["run", "-d", "--name", self.name]
        for host_port, cont_port in sorted(self.ports.items()):
            args += ["-p", f"127.0.0.1:{host_port}:{cont_port}"]
        for host_dir, cont_dir in self.mounts:
            # :z relabels for selinux hosts; harmless elsewhere
            args += ["-v", f"{host_dir}:{cont_dir}:z"]
        for k, v in sorted(self.env.items()):
            args += ["-e", f"{k}={v}"]
        if self.user is not None:
            args += ["--user", self.user]
        return [*args, self.image, *self.cmd]

    def _logs(self) -> str:
        cp = subprocess.run(
            [self.runtime, "logs", self.name], capture_output=True, text=True
        )
        return f"container logs:\n{cp.stdout}{cp.stderr}"

    def start(self) -> None:
        _wipe(self.pre_start)
        argv = [self.runtime, *self.run_args()]
        logger.info("starting %s: %s", self.what, shlex.join(argv))
        cp = subprocess.run(argv, capture_output=True, text=True)
        if cp.returncode != 0:
            raise RuntimeError(f"{self.what}: {self.runtime} run failed: {cp.stderr}")
        try:
            wait_ready(self.ready, self.what, self._logs, self.ready_timeout)
            self._make_readable()
            if self.on_started is not None:
                self.on_started()
        except BaseException:
            self.stop()
            raise

    def _make_readable(self) -> None:
        # rootless runtimes remap the in-container writer to a host subuid whose
        # 0600 files we can't read; a+r (as container root) lets host + clients
        # consume the throwaway files the container drops in a shared mount.
        for path in self.world_readable:
            argv = [self.runtime, "exec", "-u", "0", self.name]
            argv += ["chmod", "-R", "a+r", path]
            cp = subprocess.run(argv, capture_output=True, text=True)
            if cp.returncode != 0:
                raise RuntimeError(f"{self.what}: chmod {path} failed: {cp.stderr}")

    def stop(self) -> None:
        subprocess.run(
            [self.runtime, "rm", "-f", self.name], capture_output=True, text=True
        )

    def wipe_state(self) -> None:
        _wipe(self.fresh_paths)

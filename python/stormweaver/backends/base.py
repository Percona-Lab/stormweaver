import logging
import subprocess
import time
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Any

from stormweaver import log as swlog
from stormweaver.wrappers import ServerWrapper, WrapCtx, describe_exit

logger = logging.getLogger(__name__)


class DatabaseBackend(ABC):
    """Server lifecycle for one database node.

    Implementations: Postgres, MySQL.
    """

    install_dir: Path
    datadir: Path
    wrapper: ServerWrapper | None
    _proc: subprocess.Popen[bytes] | None
    _session: int

    @abstractmethod
    def initialize(self) -> None: ...

    @abstractmethod
    def add_config(self, settings: dict[str, str]) -> None: ...

    @abstractmethod
    def start(self) -> None: ...

    @abstractmethod
    def stop(self, timeout: float = 10.0) -> None: ...

    @abstractmethod
    def kill(self) -> None: ...

    @abstractmethod
    def restart(self, timeout: float = 10.0) -> None: ...

    @abstractmethod
    def is_ready(self) -> bool: ...

    @property
    @abstractmethod
    def port(self) -> int: ...

    @abstractmethod
    def createdb(self, name: str) -> None: ...

    @abstractmethod
    def connection_params(self, dbname: str) -> dict[str, Any]: ...

    @abstractmethod
    def is_running(self) -> bool: ...

    @abstractmethod
    def _server_log_path(self) -> Path: ...

    @property
    def server_log_path(self) -> Path:
        return self._server_log_path()

    def wait_ready(self, timeout: float = 60.0) -> bool:
        timeout = self._timeout(timeout)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.is_ready():
                return True
            if self._proc is not None and self._proc.poll() is not None:
                logger.error("server exited while waiting for ready")
                return False
            time.sleep(0.2)
        logger.error(
            "server not ready within %.0fs, check %s", timeout, self._server_log_path()
        )
        return False

    def _wrap_ctx(self) -> WrapCtx:
        return WrapCtx(
            node=self.datadir.name,
            datadir=self.datadir,
            run_dir=swlog.log_dir(),
            session=self._session,
        )

    def _timeout(self, timeout: float) -> float:
        if self.wrapper is not None:
            return timeout * self.wrapper.time_multiplier
        return timeout

    def _session_result(self, requested: str | None, rc: int | None) -> str:
        if requested == "kill":
            return "killed"
        if requested == "stop" and rc == 0:
            return "clean"
        return "crashed"

    def _reap(self, requested: str | None, timeout: float = 10.0) -> None:
        if self._proc is None:
            return
        try:
            rc = self._proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            logger.warning("server process did not exit, killing it")
            self._proc.kill()
            rc = self._proc.wait()
        self._proc = None
        result = self._session_result(requested, rc)
        line = (
            f"node={self.datadir.name} session={self._session}"
            f" result={result} exit={describe_exit(rc)}"
        )
        swlog.record_outcome(line)
        if result == "crashed":
            logger.warning("server session ended unexpectedly: %s", line)
        if self.wrapper is not None:
            self.wrapper.on_session_end(self._wrap_ctx(), result == "crashed")

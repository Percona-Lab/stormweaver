import logging
import signal
import subprocess
import time
from pathlib import Path
from typing import Any

from stormweaver import log as swlog
from stormweaver.backends.base import DatabaseBackend

logger = logging.getLogger(__name__)


class MySQL(DatabaseBackend):
    def __init__(
        self,
        install_dir: str | Path,
        datadir: str | Path,
        init: bool = True,
        port: int | None = None,
    ) -> None:
        self.install_dir = Path(install_dir)
        # mysqld resolves a relative --datadir against its basedir, not cwd
        # (unlike postgres), so make it absolute up front
        self.datadir = Path(datadir).absolute()
        self._port = str(port) if port else None
        self._proc: subprocess.Popen[bytes] | None = None

        if init:
            self.initialize()

    def _bin(self, name: str) -> str:
        cand = self.install_dir / "bin" / name
        if cand.exists():
            return str(cand)
        cand = self.install_dir / "sbin" / name
        if cand.exists():
            return str(cand)
        return str(self.install_dir / "bin" / name)

    @property
    def socket(self) -> Path:
        return self.datadir / "mysql.sock"

    def _defaults_file(self) -> Path:
        return self.datadir / "my.cnf"

    def _server_log_path(self) -> Path:
        # log-error goes into my.cnf and mysqld resolves relative paths
        # against its basedir, not cwd - always give it an absolute path
        run_dir = swlog.log_dir()
        if run_dir is not None:
            return (run_dir / f"server-mysql-{self.datadir.name}.log").absolute()
        return self.datadir / "server.log"

    def initialize(self) -> None:
        logger.info("Initializing datadir at %s", self.datadir)
        self.datadir.mkdir(parents=True, exist_ok=True)
        data_dir = self.datadir / "data"
        init_err = self.datadir / "init.err"
        result = subprocess.run(
            [
                self._bin("mysqld"),
                "--no-defaults",
                "--initialize-insecure",
                f"--datadir={data_dir}",
                f"--log-error={init_err}",
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"mysqld --initialize-insecure failed: {result.stderr}")
        logger.info("mysqld initialize completed successfully")

        if not self._port:
            self._port = "3306"

        with open(self._defaults_file(), "w") as f:
            f.write("[mysqld]\n")

        self.add_config(
            {
                "datadir": str(data_dir),
                "port": self._port,
                "socket": str(self.socket),
                "log-error": str(self._server_log_path()),
                "pid-file": str(self.datadir / "mysqld.pid"),
            }
        )

    @property
    def port(self) -> int:
        assert self._port is not None
        return int(self._port)

    def add_config(self, settings: dict[str, str]) -> None:
        conf_file = self._defaults_file()
        with open(conf_file, "a") as f:
            for k, v in settings.items():
                f.write(f"{k} = {v}\n")

    def start(self) -> None:
        logger.info("Starting MySQL on port %s", self._port)
        self._proc = subprocess.Popen(
            [self._bin("mysqld"), f"--defaults-file={self._defaults_file()}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        logger.info("MySQL starting, pid %s", self._proc.pid)

    def _admin(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [self._bin("mysqladmin"), "-uroot", f"--socket={self.socket}", *args],
            capture_output=True,
            text=True,
        )

    def stop(self, timeout: float = 10.0) -> None:
        logger.info("Stopping MySQL")
        self._admin("shutdown")
        if self._proc is not None:
            try:
                self._proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                logger.warning("mysqld did not stop in time, killing")
                self._proc.kill()
                self._proc.wait()
        self._proc = None

    def kill(self) -> None:
        logger.info("Killing MySQL pid %s", self._proc.pid if self._proc else None)
        if self._proc is not None:
            self._proc.send_signal(signal.SIGKILL)
            self._proc.wait()
        self._proc = None

    def restart(self, timeout: float = 10.0) -> None:
        self.stop(timeout)
        self.start()

    def is_running(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def is_ready(self) -> bool:
        return self._admin("ping").returncode == 0

    def wait_ready(self, timeout: float = 60.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.is_ready():
                return True
            time.sleep(0.2)
        return False

    def _ensure_tcp_root(self) -> None:
        # --initialize-insecure only makes root@localhost (socket auth); the
        # C++ connector talks TCP 127.0.0.1, so grant that host too
        result = subprocess.run(
            [
                self._bin("mysql"),
                "-uroot",
                f"--socket={self.socket}",
                "-e",
                "CREATE USER IF NOT EXISTS 'root'@'127.0.0.1' IDENTIFIED BY ''; "
                "GRANT ALL ON *.* TO 'root'@'127.0.0.1' WITH GRANT OPTION;",
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"ensuring tcp root failed: {result.stderr}")

    def createdb(self, name: str) -> None:
        self._ensure_tcp_root()
        result = subprocess.run(
            [
                self._bin("mysql"),
                "-uroot",
                f"--socket={self.socket}",
                "-e",
                f"CREATE DATABASE {name}",
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"createdb failed: {result.stderr}")

    def connection_params(self, dbname: str) -> dict[str, Any]:
        return {
            "host": "127.0.0.1",
            "port": self.port,
            "dbname": dbname,
            "user": "root",
        }

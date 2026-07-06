import getpass
import logging
import os
import signal
import subprocess
import time
from pathlib import Path
from typing import Any

from stormweaver import log as swlog
from stormweaver.backends.base import DatabaseBackend

logger = logging.getLogger(__name__)


class Postgres(DatabaseBackend):
    def __init__(
        self,
        install_dir: str | Path,
        datadir: str | Path,
        init: bool = True,
        port: int | None = None,
    ) -> None:
        self.install_dir = Path(install_dir)
        self.datadir = Path(datadir)
        self._port = str(port) if port else None
        self._pid: int | None = None

        if init:
            self.initialize()

    def _bin(self, name: str) -> str:
        return str(self.install_dir / "bin" / name)

    def _server_log_path(self) -> Path:
        run_dir = swlog.log_dir()
        if run_dir is not None:
            return run_dir / f"server-{self.datadir.name}.log"
        return self.datadir / "server.log"

    def initialize(self) -> None:
        logger.info("Initializing datadir at %s", self.datadir)
        self.datadir.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(
            [self._bin("initdb"), "-D", str(self.datadir), "--no-sync"],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"initdb failed: {result.stderr}")
        logger.info("initdb completed successfully")

        if self._port:
            self.add_config({"port": self._port})
        else:
            self._port = "5432"

        # Use datadir for unix socket to avoid needing /run/postgresql
        self.add_config({"unix_socket_directories": str(self.datadir.resolve())})

    @property
    def port(self) -> int:
        assert self._port is not None
        return int(self._port)

    def add_config(self, settings: dict[str, str]) -> None:
        conf_file = self.datadir / "postgresql.conf"
        with open(conf_file, "a") as f:
            for k, v in settings.items():
                f.write(f"{k} = '{v}'\n")

    def add_hba(
        self, host_type: str, database: str, user: str, address: str, method: str
    ) -> None:
        hba_file = self.datadir / "pg_hba.conf"
        with open(hba_file, "a") as f:
            f.write(f"{host_type} {database} {user} {address} {method}\n")

    def start(self) -> None:
        logger.info("Starting PostgreSQL on port %s", self._port)
        result = subprocess.run(
            [
                self._bin("pg_ctl"),
                "start",
                "-D",
                str(self.datadir),
                "-l",
                str(self._server_log_path()),
                "-w",
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"pg_ctl start failed: {result.stderr}")
        self._pid = self._read_pid()
        logger.info("PostgreSQL started")

    def _read_pid(self) -> int | None:
        pid_file = self.datadir / "postmaster.pid"
        try:
            with open(pid_file) as f:
                return int(f.readline().strip())
        except (FileNotFoundError, ValueError):  # fmt: skip
            return None

    def stop(self, timeout: float = 10) -> None:
        logger.info("Stopping PostgreSQL")
        result = subprocess.run(
            [
                self._bin("pg_ctl"),
                "stop",
                "-D",
                str(self.datadir),
                "-m",
                "fast",
                "-t",
                str(timeout),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            logger.warning("pg_ctl stop failed: %s", result.stderr)
        self._pid = None

    def kill(self) -> None:
        logger.info("Killing PostgreSQL pid %s", self._pid)
        if self._pid is not None:
            os.kill(self._pid, signal.SIGKILL)
        self._pid = None

    def restart(self, timeout: float = 10) -> None:
        self.stop(timeout)
        self.start()

    def is_running(self) -> bool:
        if self._pid is None:
            return False
        try:
            os.kill(self._pid, 0)
            return True
        except (ProcessLookupError, PermissionError):  # fmt: skip
            return False

    def is_ready(self) -> bool:
        result = subprocess.run(
            [self._bin("pg_isready"), "-h", "localhost", "-p", str(self._port)],
            capture_output=True,
            text=True,
        )
        return result.returncode == 0

    def wait_ready(self, timeout: float = 60.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.is_ready():
                return True
            time.sleep(0.2)
        return False

    def createdb(self, name: str) -> None:
        result = subprocess.run(
            [self._bin("createdb"), "-h", "localhost", "-p", str(self._port), name],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"createdb failed: {result.stderr}")

    def dropdb(self, name: str) -> None:
        subprocess.run(
            [self._bin("dropdb"), "-h", "localhost", "-p", str(self._port), name],
            capture_output=True,
            text=True,
        )

    def basebackup(
        self, target_datadir: str | Path, extra_args: list[str] | None = None
    ) -> None:
        args = [
            self._bin("pg_basebackup"),
            "-D",
            str(target_datadir),
            "-h",
            "localhost",
            "-p",
            str(self._port),
            "--no-sync",
        ]
        if extra_args:
            args.extend(extra_args)
        result = subprocess.run(args, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"pg_basebackup failed: {result.stderr}")

    def connection_params(self, dbname: str) -> dict[str, Any]:
        user = os.environ.get("PGUSER") or getpass.getuser()
        return {
            "host": "localhost",
            "port": self.port,
            "dbname": dbname,
            "user": user,
        }

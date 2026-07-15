import getpass
import logging
import os
import shutil
import signal
import subprocess
import tempfile
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from typing import Any

from stormweaver import log as swlog
from stormweaver.backends.base import DatabaseBackend
from stormweaver.wrappers import ServerWrapper

logger = logging.getLogger(__name__)


class Postgres(DatabaseBackend):
    def __init__(
        self,
        install_dir: str | Path,
        datadir: str | Path,
        init: bool = True,
        port: int | None = None,
        wrapper: ServerWrapper | None = None,
        archive_dir: str | Path | None = None,
        initdb_args: list[str] | None = None,
    ) -> None:
        self.install_dir = Path(install_dir)
        self.datadir = Path(datadir)
        self.archive_dir = (
            Path(archive_dir)
            if archive_dir is not None
            else self.datadir.parent / f"{self.datadir.name}_archive"
        )
        self._port = str(port) if port else None
        self.wrapper = wrapper
        self._proc: subprocess.Popen[bytes] | None = None
        self._session = 0
        self._initdb_args = list(initdb_args) if initdb_args else []

        if init:
            self.initialize()

    def _bin(self, name: str) -> str:
        return str(self.install_dir / "bin" / name)

    def _server_log_path(self) -> Path:
        run_dir = swlog.log_dir()
        if run_dir is not None:
            return run_dir / f"server-{self.datadir.name}.log"
        return self.datadir / f"server-{self.datadir.name}.log"

    def initialize(self) -> None:
        logger.info("Initializing datadir at %s", self.datadir)
        self.datadir.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(
            [
                self._bin("initdb"),
                "-D",
                str(self.datadir),
                "--no-sync",
                *self._initdb_args,
            ],
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

    def enable_archiving(self) -> None:
        self.archive_dir.mkdir(parents=True, exist_ok=True)
        self.archive_dir.chmod(0o700)
        self.add_config(
            {
                "archive_mode": "on",
                "archive_command": f'cp "%p" "{self.archive_dir.resolve()}/%f"',
            }
        )

    def set_signal(self, kind: str) -> None:
        if kind not in ("recovery", "standby"):
            raise ValueError(f"unknown signal kind: {kind}")
        # PGDATA files are 0600; touch() would honor umask (0644)
        sig = self.datadir / f"{kind}.signal"
        sig.touch()
        sig.chmod(0o600)

    def enable_restoring(
        self, source: Postgres | str | Path, signal: str | None = None
    ) -> None:
        archive = source.archive_dir if isinstance(source, Postgres) else Path(source)
        if not archive.is_dir():
            raise RuntimeError(f"archive dir does not exist: {archive}")
        self.add_config({"restore_command": f'cp "{archive.resolve()}/%f" "%p"'})
        if signal is not None:
            self.set_signal(signal)

    def move_wal_to_archive(self) -> None:
        """Relocate pg_wal contents into archive_dir; server must be stopped."""
        wal = self.datadir / "pg_wal"
        self.archive_dir.mkdir(parents=True, exist_ok=True)
        shutil.copytree(wal, self.archive_dir, dirs_exist_ok=True)
        shutil.rmtree(wal)
        wal.mkdir()
        wal.chmod(0o700)
        self.archive_dir.chmod(0o700)

    @contextmanager
    def preserve_config(self) -> Iterator[Path]:
        """Save postgresql.conf aside, yield the saved path, restore on exit.

        The yielded copy is what pg_rewind's --config-file wants: a valid
        conf that survives the datadir copy being overwritten.
        """
        conf = self.datadir / "postgresql.conf"
        tmp = Path(tempfile.mkdtemp(prefix="sw-conf-", dir="/tmp"))
        saved = tmp / "postgresql.conf"
        shutil.copy2(conf, saved)
        try:
            yield saved
        finally:
            shutil.move(str(saved), conf)
            shutil.rmtree(tmp, ignore_errors=True)

    def add_hba(
        self, host_type: str, database: str, user: str, address: str, method: str
    ) -> None:
        hba_file = self.datadir / "pg_hba.conf"
        with open(hba_file, "a") as f:
            f.write(f"{host_type} {database} {user} {address} {method}\n")

    def _read_pid(self) -> int | None:
        pid_file = self.datadir / "postmaster.pid"
        try:
            with open(pid_file) as f:
                return int(f.readline().strip())
        except (FileNotFoundError, ValueError):  # fmt: skip
            return None

    def _server_pid(self) -> int | None:
        if self.wrapper is None and self._proc is not None:
            return self._proc.pid
        return self._read_pid()

    def start(self) -> None:
        if self._proc is not None:
            raise RuntimeError("postgres already started")
        self._session += 1
        logger.info(
            "Starting PostgreSQL on port %s (session %d)", self._port, self._session
        )
        wrapper = self.wrapper or ServerWrapper()
        cmd = [self._bin("postgres"), "-D", str(self.datadir)]
        # postgres logs to stderr when started directly; under a wrapper the
        # tool's own stderr shares this file (same stream, can't split)
        with open(self._server_log_path(), "ab") as log_file:
            self._proc = wrapper.spawn(
                cmd, self._wrap_ctx(), stdout=log_file, stderr=subprocess.STDOUT
            )
        logger.info("PostgreSQL starting, pid %s", self._proc.pid)

    def stop(self, timeout: float = 10, mode: str = "fast") -> None:
        if mode not in ("smart", "fast", "immediate"):
            raise ValueError(f"unknown stop mode: {mode}")
        logger.info("Stopping PostgreSQL (mode=%s)", mode)
        timeout = self._timeout(timeout)
        result = subprocess.run(
            [
                self._bin("pg_ctl"),
                "stop",
                "-D",
                str(self.datadir),
                "-m",
                mode,
                "-t",
                str(int(timeout)),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            logger.warning("pg_ctl stop failed: %s", result.stderr)
        self._reap("stop", timeout)

    def kill(self) -> None:
        pid = self._server_pid()
        if pid is None and self._proc is not None:
            pid = self._proc.pid
        logger.info("Killing PostgreSQL pid %s", pid)
        if pid is not None:
            os.kill(pid, signal.SIGKILL)
        # a wrapper (rr) may need a while to finalize its trace
        self._reap("kill", self._timeout(30.0))

    def restart(self, timeout: float = 10) -> None:
        self.stop(timeout)
        self.start()

    def promote(self, timeout: float = 30) -> None:
        result = subprocess.run(
            [
                self._bin("pg_ctl"),
                "promote",
                "-D",
                str(self.datadir),
                "-w",
                "-t",
                str(int(self._timeout(timeout))),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"pg_ctl promote failed: {result.stderr}")

    def is_running(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def is_ready(self) -> bool:
        result = subprocess.run(
            [self._bin("pg_isready"), "-h", "localhost", "-p", str(self._port)],
            capture_output=True,
            text=True,
        )
        return result.returncode == 0

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
        self,
        target_datadir: str | Path,
        extra_args: list[str] | None = None,
        incremental: str | Path | None = None,
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
        if incremental:
            args.extend(["-i", str(incremental)])
        if extra_args:
            args.extend(extra_args)
        result = subprocess.run(args, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"pg_basebackup failed: {result.stderr}")

    def combinebackup(self, backups: list[str | Path], output: str | Path) -> None:
        args = [
            self._bin("pg_combinebackup"),
            *[str(b) for b in backups],
            "-o",
            str(output),
        ]
        result = subprocess.run(args, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"pg_combinebackup failed: {result.stderr}")

    def connection_params(self, dbname: str) -> dict[str, Any]:
        user = os.environ.get("PGUSER") or getpass.getuser()
        return {
            "host": "localhost",
            "port": self.port,
            "dbname": dbname,
            "user": user,
        }

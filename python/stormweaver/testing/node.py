import itertools
import logging
import re
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
from collections.abc import Sequence
from pathlib import Path
from types import TracebackType
from typing import Any, Self

import stormweaver._stormweaver as _sw
from stormweaver import events
from stormweaver.backends.base import DatabaseBackend
from stormweaver.backends.mysql import MySQL
from stormweaver.backends.postgres import Postgres
from stormweaver.testing import cluster, files, process
from stormweaver.testing.util import alloc_port, ensure_logging
from stormweaver.wrappers import ServerWrapper

Rows = list[list[str | None]]

# spdlog logger names are get-or-create per process; must be unique
_conn_seq = itertools.count(1)


def _to_params(params: Sequence[Any] | None) -> list[Any] | None:
    # empty params -> None -> multi-statement capable path
    return list(params) if params else None


def _tail(path: Path, lines: int = 50) -> str:
    # server logs can be huge, only read a bounded window from the end
    with open(path, "rb") as f:
        f.seek(0, 2)
        size = f.tell()
        f.seek(max(0, size - 65536))
        data = f.read()
    return "\n".join(data.decode(errors="replace").splitlines()[-lines:])


def _rows_text(rows: Rows) -> str:
    return "\n".join(",".join("NULL" if v is None else v for v in row) for row in rows)


def _assert_pass(log: logging.Logger, kind: str, **fields: str) -> None:
    events.emit("ASSERT", {"status": "pass", "kind": kind, **fields}, logger=log)


def _assert_fail(
    log: logging.Logger,
    kind: str,
    *,
    node: TestNode | None = None,
    rows: Rows | None = None,
    **fields: str,
) -> None:
    events.emit(
        "ASSERT",
        {"status": "fail", "kind": kind, **fields},
        level=logging.ERROR,
        logger=log,
    )
    # context dumps must never mask the original failure
    try:
        if sys.exc_info()[0] is not None:
            tb = traceback.format_exc()
        else:
            # no exception raised yet: capture the assertion's call site instead
            tb = "".join(traceback.format_stack()[:-2])
        events.dump("traceback", tb, logger=log)
        if rows is not None:
            events.dump("result", _rows_text(rows), logger=log, rows=str(len(rows)))
        if node is not None:
            try:
                events.dump(
                    "server-log-tail",
                    _tail(node.server_log),
                    logger=log,
                    node=node.name,
                )
            except OSError:
                log.warning("could not read server log tail", exc_info=True)
    except Exception:
        log.warning("failure context dump failed", exc_info=True)


class TestConn:
    """Thin test-facing wrapper over a LoggedSQL connection.

    A connection held across a node restart fails with serverGone; call
    reconnect() to re-establish it.
    """

    def __init__(
        self, raw: Any, name: str = "conn", node: TestNode | None = None
    ) -> None:
        self.raw = raw
        self.name = name
        self.node = node
        self._log = logging.getLogger(f"test.{name}")

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        # dropping the last ref destroys the LoggedSQL and closes the session
        self.raw = None

    def _live(self) -> Any:
        if self.raw is None:
            raise RuntimeError("connection closed")
        return self.raw

    def reconnect(self) -> None:
        self._live().reconnect()

    def sql(self, query: str, params: Sequence[Any] | None = None) -> Any:
        return self._live().execute(query, _to_params(params))

    def safe_sql(self, query: str, params: Sequence[Any] | None = None) -> Rows:
        try:
            rows: Rows = self._live().safe_execute(query, _to_params(params)).rows()
        except _sw.SqlError as e:
            _assert_fail(
                self._log, "safe_sql", node=self.node, query=query, error=str(e)
            )
            raise
        _assert_pass(self._log, "safe_sql", query=query, rows=str(len(rows)))
        return rows

    def sql_value(self, query: str, params: Sequence[Any] | None = None) -> str | None:
        rows = self.safe_sql(query, params)
        if len(rows) != 1 or len(rows[0]) != 1:
            cols = len(rows[0]) if rows else 0
            _assert_fail(
                self._log,
                "sql_value",
                node=self.node,
                rows=rows,
                query=query,
                expected="1x1",
                actual=f"{len(rows)}x{cols}",
            )
            raise AssertionError(
                f"expected 1x1 result, got {len(rows)}x{cols}: {query}"
            )
        return rows[0][0]

    def expect_error(
        self, pattern: str, query: str, params: Sequence[Any] | None = None
    ) -> Any:
        res = self.sql(query, params)
        if res.success():
            _assert_fail(
                self._log,
                "expect_error",
                node=self.node,
                rows=res.rows(),
                pattern=pattern,
                query=query,
                actual="success",
            )
            raise AssertionError(
                f"query succeeded, expected error {pattern!r}: {query}"
            )
        if not re.search(pattern, res.error_message):
            _assert_fail(
                self._log,
                "expect_error",
                node=self.node,
                pattern=pattern,
                query=query,
                actual=res.error_message,
            )
            raise AssertionError(
                f"error {res.error_message!r} does not match {pattern!r}"
            )
        _assert_pass(
            self._log,
            "expect_error",
            pattern=pattern,
            query=query,
            error=res.error_message,
        )
        return res


class TestNode:
    """Server + default connection + TAP-style conveniences."""

    def __init__(self, db: DatabaseBackend, dbname: str, name: str = "node") -> None:
        self.db = db
        self.dbname = dbname
        self.name = name
        self._default: TestConn | None = None
        self._owned_dir: Path | None = None
        self._log = logging.getLogger(f"test.{name}")

    def _node_event(self, event: str) -> None:
        # "event" collides with emit()'s first positional, pass via fields dict
        events.emit(
            "NODE",
            {
                "event": event,
                "name": self.name,
                "port": str(self.db.port),
                "datadir": str(self.db.datadir),
            },
            logger=self._log,
        )

    # lifecycle -- every transition invalidates the default connection

    def start(self) -> None:
        self._default = None
        self.db.start()
        if not self.db.wait_ready():
            _assert_fail(self._log, "node_start", node=self, name=self.name)
            raise RuntimeError(
                f"server did not become ready, log: {self.db.server_log_path}"
            )
        self._node_event("start")

    def stop(self, timeout: float = 10.0) -> None:
        self._default = None
        self.db.stop(timeout)
        self._node_event("stop")

    def kill(self) -> None:
        self._default = None
        self.db.kill()
        self._node_event("kill")

    def restart(self, timeout: float = 10.0) -> None:
        self.stop(timeout)
        self.start()

    def createdb(self, dbname: str) -> None:
        self.db.createdb(dbname)

    @property
    def port(self) -> int:
        return self.db.port

    @property
    def datadir(self) -> Path:
        return self.db.datadir

    @property
    def server_log(self) -> Path:
        return self.db.server_log_path

    def log_offset(self) -> int:
        try:
            return self.server_log.stat().st_size
        except FileNotFoundError:
            return 0

    def wait_for_log(
        self, pattern: str, offset: int = 0, timeout: float = 30.0
    ) -> None:
        deadline = time.monotonic() + timeout
        chunk = ""
        while True:
            if self.server_log.exists():
                with open(self.server_log, errors="replace") as f:
                    f.seek(offset)
                    chunk = f.read()
                if re.search(pattern, chunk):
                    _assert_pass(self._log, "wait_for_log", pattern=pattern)
                    return
            if time.monotonic() >= deadline:
                _assert_fail(self._log, "wait_for_log", node=self, pattern=pattern)
                raise TimeoutError(
                    f"pattern {pattern!r} not in {self.server_log} "
                    f"after {timeout}s, tail:\n{chunk[-2000:]}"
                )
            time.sleep(0.5)

    def poll_until(
        self,
        query: str,
        expected: str | None = "t",
        params: Sequence[Any] | None = None,
        timeout: float = 30.0,
        interval: float = 0.5,
    ) -> None:
        """Poll query until its single value equals expected.

        Booleans render per server: pg "t"/"f", mysql "1"/"0"; pass
        expected accordingly.
        """
        deadline = time.monotonic() + timeout
        last: str | None = None
        attempts = 0
        while time.monotonic() < deadline:
            attempts += 1
            last = self.sql_value(query, params)
            if last == expected:
                _assert_pass(
                    self._log,
                    "poll_until",
                    query=query,
                    expected="" if expected is None else expected,
                    attempts=str(attempts),
                )
                return
            time.sleep(interval)
        _assert_fail(
            self._log,
            "poll_until",
            node=self,
            query=query,
            expected="" if expected is None else expected,
            actual="" if last is None else last,
            attempts=str(attempts),
        )
        raise TimeoutError(f"{query} did not reach {expected!r}, last value {last!r}")

    def close(self) -> None:
        self._default = None
        try:
            if self.db.is_running():
                self.db.stop()
        finally:
            if self._owned_dir is not None:
                shutil.rmtree(self._owned_dir, ignore_errors=True)

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.close()

    # connections

    def raw_connect(self, log_name: str | None = None, *, db: str | None = None) -> Any:
        raise NotImplementedError

    def connect(self, db: str | None = None, log_name: str | None = None) -> TestConn:
        name = log_name or f"{self.name}-conn-{next(_conn_seq)}"
        return TestConn(self.raw_connect(name, db=db), name=name, node=self)

    def _conn(self) -> TestConn:
        if self._default is None:
            self._default = self.connect()
        return self._default

    # SQL conveniences on the default connection

    def sql(self, query: str, params: Sequence[Any] | None = None) -> Any:
        return self._conn().sql(query, params)

    def safe_sql(self, query: str, params: Sequence[Any] | None = None) -> Rows:
        return self._conn().safe_sql(query, params)

    def sql_value(self, query: str, params: Sequence[Any] | None = None) -> str | None:
        return self._conn().sql_value(query, params)

    def expect_error(
        self, pattern: str, query: str, params: Sequence[Any] | None = None
    ) -> Any:
        return self._conn().expect_error(pattern, query, params)

    # external binaries

    def bin_env(self) -> dict[str, str]:
        return {}

    def run_bin(
        self, name: str, *args: str | Path, **kw: Any
    ) -> subprocess.CompletedProcess[str]:
        env = self.bin_env() | (kw.pop("env", None) or {})
        return process.run_bin(self.db.install_dir, name, *args, env=env, **kw)


def _fresh_dir(name: str) -> Path:
    # /tmp keeps unix socket paths under the 107-byte limit
    return Path(tempfile.mkdtemp(prefix=f"sw-{name}-", dir="/tmp"))


class PgTestNode(TestNode):
    db: Postgres

    @classmethod
    def fresh(
        cls,
        install_dir: str | Path,
        *,
        name: str = "node",
        dbname: str = "postgres",
        config: dict[str, str] | None = None,
        port: int | None = None,
        basedir: str | Path | None = None,
        wrapper: ServerWrapper | None = None,
    ) -> Self:
        ensure_logging()
        owned = basedir is None
        base = Path(basedir) if basedir is not None else _fresh_dir(name)
        try:
            pg = Postgres(
                install_dir=install_dir,
                datadir=base / name,
                port=port or alloc_port(),
                wrapper=wrapper,
            )
        except BaseException:
            if owned:
                shutil.rmtree(base, ignore_errors=True)
            raise
        if config:
            pg.add_config(config)
        node = cls(pg, dbname, name)
        node._node_event("init")
        if owned:
            node._owned_dir = base
        try:
            node.start()
            if dbname != "postgres":
                pg.createdb(dbname)
        except BaseException:
            node.close()
            raise
        return node

    @classmethod
    def from_backup(
        cls,
        backup_dir: str | Path,
        primary: PgTestNode,
        *,
        name: str = "standby",
        port: int | None = None,
    ) -> PgTestNode:
        return cluster.standby_from_backup(
            cls, backup_dir, primary, name=name, port=port
        )

    def basebackup(self, dest: str | Path | None = None) -> Path:
        return cluster.take_basebackup(self, dest)

    def promote(self) -> None:
        self.db.promote()
        self._node_event("promote")

    def wait_for_catchup(self, standby: PgTestNode, timeout: float = 60.0) -> None:
        cluster.wait_for_catchup(self, standby, timeout)

    def rejoin_as_standby(self, new_primary: PgTestNode) -> None:
        cluster.rejoin_as_standby(self, new_primary)

    def raw_connect(self, log_name: str | None = None, *, db: str | None = None) -> Any:
        return _sw.connect_pg(
            log_name=log_name or f"{self.name}-conn-{next(_conn_seq)}",
            **self.db.connection_params(db or self.dbname),
        )

    def bin_env(self) -> dict[str, str]:
        return {
            "PGHOST": "localhost",
            "PGPORT": str(self.port),
            "PGDATABASE": self.dbname,
        }

    def psql(
        self,
        sql: str | None = None,
        *,
        file: str | Path | None = None,
        db: str | None = None,
        on_error_stop: bool = True,
    ) -> subprocess.CompletedProcess[str]:
        if sql is not None and file is not None:
            raise ValueError("pass sql or file, not both")
        args: list[str | Path] = ["-X"]
        if on_error_stop:
            args += ["-v", "ON_ERROR_STOP=1"]
        if db:
            args += ["-d", db]
        if file is not None:
            args += ["-f", file]
        return self.run_bin("psql", *args, input=sql)

    def relation_filepath(self, table: str) -> Path:
        rel = self.sql_value("SELECT pg_relation_filepath($1)", (table,))
        assert rel is not None, f"relation {table!r} has no storage"
        return self.datadir / rel

    def read_relation_file(self, table: str) -> bytes:
        """First segment only; CHECKPOINT (or restart) first so data is on disk."""
        return files.read_file_bytes(self.relation_filepath(table))


class MySqlTestNode(TestNode):
    db: MySQL

    @classmethod
    def fresh(
        cls,
        install_dir: str | Path,
        *,
        name: str = "mynode",
        dbname: str = "test",
        config: dict[str, str] | None = None,
        port: int | None = None,
        basedir: str | Path | None = None,
        wrapper: ServerWrapper | None = None,
    ) -> Self:
        ensure_logging()
        owned = basedir is None
        base = Path(basedir) if basedir is not None else _fresh_dir(name)
        try:
            my = MySQL(
                install_dir=install_dir,
                datadir=base / name,
                port=port or alloc_port(),
                wrapper=wrapper,
            )
        except BaseException:
            if owned:
                shutil.rmtree(base, ignore_errors=True)
            raise
        if config:
            my.add_config(config)
        node = cls(my, dbname, name)
        node._node_event("init")
        if owned:
            node._owned_dir = base
        try:
            node.start()
            # mysql has no default user database
            node.createdb(dbname)
        except BaseException:
            node.close()
            raise
        return node

    def raw_connect(self, log_name: str | None = None, *, db: str | None = None) -> Any:
        return _sw.connect_mysql(
            log_name=log_name or f"{self.name}-conn-{next(_conn_seq)}",
            **self.db.connection_params(db or self.dbname),
        )

    def bin_env(self) -> dict[str, str]:
        return {"MYSQL_HOST": "127.0.0.1", "MYSQL_TCP_PORT": str(self.port)}

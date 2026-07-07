import argparse
import difflib
import getpass
import itertools
import logging
import os
import shutil
import time
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from pathlib import Path
from typing import Any

import stormweaver._stormweaver as _sw
from stormweaver import log as swlog
from stormweaver.backends.base import DatabaseBackend
from stormweaver.backends.mysql import MySQL
from stormweaver.backends.postgres import Postgres
from stormweaver.config import Config
from stormweaver.tde import init_tde_globally, init_tde_only_for_db
from stormweaver.workload import Workload
from stormweaver.wrappers import ServerWrapper, make_wrapper

logger = logging.getLogger(__name__)


def parse(
    args: argparse.Namespace,
    extend: Callable[[argparse.ArgumentParser], None] | None = None,
) -> argparse.Namespace:
    """Parse common scenario options out of args.extra (cli passthrough)."""
    parser = argparse.ArgumentParser(prog="scenario")
    parser.add_argument("--duration", type=int, default=10, help="workload seconds")
    parser.add_argument("--workers", type=int, default=5, help="number of workers")
    parser.add_argument("--repeat", type=int, default=5, help="workload cycles")
    parser.add_argument("--tde", choices=["on", "on_wal", "off"], default="off")
    parser.add_argument("--pgsm", choices=["on", "off"], default="off")
    parser.add_argument("--clear-logs", action="store_true")
    wrap = parser.add_mutually_exclusive_group()
    wrap.add_argument(
        "--wrapper", choices=["rr", "valgrind"], help="run servers under a preset tool"
    )
    wrap.add_argument(
        "--wrapper-cmd", help="run servers under an arbitrary command prefix"
    )
    parser.add_argument(
        "--wrapper-arg",
        action="append",
        default=[],
        help="extra argument for the wrapper tool, repeatable",
    )
    parser.add_argument(
        "--keep-traces", action="store_true", help="keep traces of clean sessions too"
    )
    if extend:
        extend(parser)
    opts = parser.parse_args(args.extra)

    # replaces the preset name with a ready-to-use wrapper object
    opts.wrapper = make_wrapper(
        opts.wrapper, opts.wrapper_cmd, opts.wrapper_arg, opts.keep_traces
    )

    opts.config = Config.load(args.config)
    opts.install_dir = args.install_dir or opts.config.pgroot
    if not opts.install_dir:
        raise RuntimeError(
            "database install dir required: use -i or set pgroot in config"
        )

    if opts.clear_logs:
        _clear_old_logs()

    return opts


def _clear_old_logs(base_dir: str | Path = "logs") -> None:
    # never delete the active run dir, the cli already logs into it
    active = swlog.log_dir()
    base = Path(base_dir)
    if not base.is_dir():
        return
    for entry in base.iterdir():
        if active is not None and entry.resolve() == active.resolve():
            continue
        logger.warning("clearing old log dir %s", entry)
        shutil.rmtree(entry, ignore_errors=True)


def fresh_dir(*paths: str | Path) -> None:
    """Delete and recreate each dir, warns when old contents are dropped."""
    for raw in paths:
        path = Path(raw)
        if path.is_dir():
            logger.warning("deleting old dir %s", path)
            shutil.rmtree(path)
        elif path.exists():
            logger.warning("deleting old dir %s", path)
            path.unlink()
        path.mkdir(parents=True)


def wait_for_log(
    path: str | Path, pattern: str, timeout: float, offset: int = 0
) -> bool:
    """Poll a log file until pattern shows up past offset, False on timeout."""
    path = Path(path)
    deadline = time.monotonic() + timeout
    while True:
        if path.exists():
            with open(path, errors="replace") as f:
                f.seek(offset)
                if pattern in f.read():
                    return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.5)


def compare_checksums(actual: str | Path, expected: str | Path, what: str) -> None:
    """Compare two checksum files, raise with a unified diff on mismatch."""
    a, e = Path(actual), Path(expected)
    actual_text = a.read_text()
    expected_text = e.read_text()
    if actual_text != expected_text:
        diff = "".join(
            difflib.unified_diff(
                expected_text.splitlines(keepends=True),
                actual_text.splitlines(keepends=True),
                fromfile=str(e),
                tofile=str(a),
            )
        )
        raise RuntimeError(f"checksum mismatch for {what}:\n{diff}")


STANDARD_ACTIONS = [
    ("checkpoint", "CHECKPOINT;", 10, False),
    ("vacuum_full_table", "VACUUM FULL {table};", 20, True),
    ("truncate_table", "TRUNCATE {table};", 20, True),
    ("reindex_table", "REINDEX TABLE {table};", 20, True),
]

# worker/context names double as process-wide spdlog logger names
# (get-or-create), reuse silently appends to the old log file
_context_seq = itertools.count(1)


def _base_registry(flavor: str = "pg", standard_actions: bool = False) -> Any:
    # copy, never mutate the process-wide default registry
    registry = _sw.ActionRegistry()
    registry.use(_sw.default_action_registry(flavor))
    for name in ("create_partition", "drop_partition"):
        if registry.has(name):
            registry.remove(name)
    if standard_actions:
        for name, sql, weight, per_table in STANDARD_ACTIONS:
            if per_table:
                registry.make_custom_table_sql(name, sql, weight)
            else:
                registry.make_custom_sql(name, sql, weight)
    return registry


class ScenarioContext:
    def __init__(
        self,
        opts: argparse.Namespace,
        db: DatabaseBackend,
        dbname: str,
        registry: Any,
        worker_setup: Callable[[Any, int], None] | None = None,
    ) -> None:
        self.opts = opts
        self.db = db
        self.dbname = dbname
        self.registry = registry
        self.metadata = _sw.Metadata()
        self.datadir = str(db.datadir)
        self._name_prefix = f"ctx{next(_context_seq)}-"
        self._worker_seq = 0
        action_config = _sw.AllConfig()
        action_config.ddl.access_methods = self._access_methods()
        # one cycle per run(), scenarios own the repeat loop
        self.workload = Workload(
            workers=opts.workers,
            duration=opts.duration,
            registry=registry,
            metadata=self.metadata,
            node_factory=self.connect,
            action_config=action_config,
            worker_name_prefix=self._name_prefix,
            worker_setup=worker_setup,
        )

    def _access_methods(self) -> list[str]:
        raise NotImplementedError

    def connect(self, log_name: str = "scenario") -> Any:
        raise NotImplementedError

    def make_worker(self, name: str) -> Any:
        self._worker_seq += 1
        wname = f"{self._name_prefix}{name}-{self._worker_seq}"
        return _sw.Worker(
            wname, lambda: self.connect(wname), _sw.WorkloadParams(), self.metadata
        )

    def restart_and_wait(self, timeout: float = 10) -> None:
        self.db.restart(timeout)
        if not self.db.wait_ready():
            raise RuntimeError("server did not become ready after restart")

    def validate_metadata_or_warn(self) -> None:
        # Known limitation: metadata may diverge under concurrent DDL until
        # the metadata rework; do not fail scenarios on this.
        if not self.make_worker("validator").validate_metadata():
            logger.warning("metadata validation failed (known limitation, ignored)")


class PgContext(ScenarioContext):
    db: Postgres

    def __init__(
        self,
        opts: argparse.Namespace,
        db: Postgres,
        dbname: str,
        registry: Any,
        keyring: Path | None,
        conn_settings: Callable[[Any], None] | None,
        worker_setup: Callable[[Any, int], None] | None,
    ) -> None:
        self.keyring = keyring
        self._conn_settings = conn_settings
        self._user = os.environ.get("PGUSER") or getpass.getuser()
        super().__init__(opts, db, dbname, registry, worker_setup)

    @property
    def pg(self) -> Postgres:
        return self.db

    def _access_methods(self) -> list[str]:
        if self.opts.tde != "off":
            return ["tde_heap", "heap"]
        return ["heap"]

    def connect(self, log_name: str = "scenario") -> Any:
        conn = _sw.connect_pg(
            host="localhost",
            port=self.db.port,
            dbname=self.dbname,
            user=self._user,
            log_name=log_name,
        )
        if self.opts.tde != "off":
            conn.execute("SET default_table_access_method = tde_heap;")
        if self._conn_settings:
            self._conn_settings(conn)
        return conn


@contextmanager
def single_pg(
    opts: argparse.Namespace,
    *,
    archive: bool = False,
    extra_config: dict[str, str] | None = None,
    conn_settings: Callable[[Any], None] | None = None,
    db_setup: Callable[[Any], None] | None = None,
    initial_tables: int = 5,
    dbname: str = "testdb",
    worker_setup: Callable[[Any, int], None] | None = None,
    datadir_name: str = "primary",
    wrapper: ServerWrapper | None = None,
) -> Iterator[PgContext]:
    """Fresh single-primary postgres with standard actions and a ready workload."""
    datadir = opts.config.datadir(datadir_name)
    shutil.rmtree(datadir, ignore_errors=True)

    keyring = None
    if opts.tde != "off":
        # postgres chdir's into the datadir, keyring path must be absolute
        keyring = Path(datadir).resolve().parent / f"{datadir_name}_keyring.per"
        keyring.parent.mkdir(parents=True, exist_ok=True)
        keyring.unlink(missing_ok=True)

    preload = []
    if opts.tde != "off":
        preload.append("pg_tde")
    if opts.pgsm == "on":
        preload.append("pg_stat_monitor")

    settings = {
        "log_min_messages": "warning",
        "max_connections": "100",
        "shared_buffers": "256MB",
    }
    if preload:
        settings["shared_preload_libraries"] = ",".join(preload)
    if archive:
        Path("archive").mkdir(exist_ok=True)
        settings |= {
            "summarize_wal": "on",
            "archive_mode": "on",
            "archive_command": f"cp %p {Path('archive').resolve()}/%f",
            "max_wal_senders": "3",
        }
    if extra_config:
        settings |= extra_config

    pg = Postgres(
        install_dir=opts.install_dir,
        datadir=datadir,
        port=opts.config.free_port(),
        wrapper=wrapper or getattr(opts, "wrapper", None),
    )
    pg.add_config(settings)
    pg.start()
    try:
        if not pg.wait_ready():
            raise RuntimeError("postgres did not become ready in time")
        pg.createdb(dbname)

        registry = _base_registry("pg", standard_actions=True)
        ctx = PgContext(
            opts, pg, dbname, registry, keyring, conn_settings, worker_setup
        )

        if opts.tde == "on":
            init_tde_only_for_db(ctx.connect("tde-init"), str(keyring))
        elif opts.tde == "on_wal":
            init_tde_globally(ctx.connect("tde-init"), str(keyring))

        setup_worker = ctx.make_worker("setup")
        if db_setup:
            db_setup(setup_worker)
        else:
            setup_worker.create_random_tables(initial_tables)

        # init may have used ALTER SYSTEM (e.g. wal_encrypt), apply it
        ctx.restart_and_wait()

        yield ctx
    finally:
        pg.stop()


class MySqlContext(ScenarioContext):
    db: MySQL

    @property
    def my(self) -> MySQL:
        return self.db

    def _access_methods(self) -> list[str]:
        return ["InnoDB"]

    def connect(self, log_name: str = "scenario") -> Any:
        return _sw.connect_mysql(
            host="127.0.0.1",
            port=self.db.port,
            dbname=self.dbname,
            user="root",
            log_name=log_name,
        )


@contextmanager
def single_mysql(
    opts: argparse.Namespace,
    *,
    extra_config: dict[str, str] | None = None,
    db_setup: Callable[[Any], None] | None = None,
    initial_tables: int = 5,
    dbname: str = "testdb",
    worker_setup: Callable[[Any, int], None] | None = None,
    datadir_name: str = "primary_mysql",
    wrapper: ServerWrapper | None = None,
) -> Iterator[MySqlContext]:
    """Fresh single mysql server with a ready workload."""
    datadir = opts.config.datadir(datadir_name)
    shutil.rmtree(datadir, ignore_errors=True)

    my = MySQL(
        install_dir=opts.install_dir,
        datadir=datadir,
        port=opts.config.free_port(),
        wrapper=wrapper or getattr(opts, "wrapper", None),
    )
    my.add_config({"max_connections": "200"} | (extra_config or {}))
    my.start()
    try:
        if not my.wait_ready():
            raise RuntimeError("mysqld did not become ready in time")
        my.createdb(dbname)

        registry = _base_registry("mysql")
        ctx = MySqlContext(opts, my, dbname, registry, worker_setup)

        setup_worker = ctx.make_worker("setup")
        if db_setup:
            db_setup(setup_worker)
        else:
            setup_worker.create_random_tables(initial_tables)

        yield ctx
    finally:
        my.stop()

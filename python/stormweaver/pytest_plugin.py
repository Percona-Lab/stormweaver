import itertools
import os
import shutil
import tempfile
from collections.abc import Callable, Iterator
from pathlib import Path

import pytest

import stormweaver as sw
from stormweaver import log as swlog

# not xdist-safe: every xdist worker process restarts this counter at 26400;
# offset by PYTEST_XDIST_WORKER if parallelism is ever adopted
_ports = itertools.count(26400)
_log_ids = itertools.count(1)


def _detect_pg_dir() -> str | None:
    env = os.environ.get("STORMWEAVER_PG_DIR")
    if env:
        return env
    candidates = [
        Path.home() / ".local/share/stormweaver-pg18",
        Path("/usr/lib/postgresql18"),
    ]
    base = Path("/usr/lib/postgresql")
    if base.exists():
        candidates += sorted(base.iterdir(), key=lambda p: p.name, reverse=True)
    for c in candidates:
        if (c / "bin" / "initdb").exists():
            return str(c)
    return None


@pytest.fixture(scope="session")
def pg_install_dir() -> str:
    d = _detect_pg_dir()
    if d is None:
        pytest.skip("no postgres installation (set STORMWEAVER_PG_DIR)")
    return d


def _server(pg_install_dir: str) -> Iterator[sw.Postgres]:
    # /tmp-based short path: unix socket dirs must stay under 107 bytes
    datadir = tempfile.mkdtemp(prefix="sw-pytest-", dir="/tmp")
    pg: sw.Postgres | None = None
    try:
        pg = sw.Postgres(
            install_dir=pg_install_dir,
            datadir=str(Path(datadir) / "data"),
            port=next(_ports),
        )
        pg.start()
        assert pg.wait_ready(), "postgres did not become ready"
        yield pg
    finally:
        if pg is not None:
            pg.stop()
        shutil.rmtree(datadir, ignore_errors=True)


@pytest.fixture(scope="session")
def sw_log_dir() -> Path:
    # always create the pytest run dir: earlier unit tests may have left
    # log_dir() pointing at a throwaway tmp_path
    return swlog.init_run_logging("pytest")


@pytest.fixture
def postgres_server(pg_install_dir: str, sw_log_dir: Path) -> Iterator[sw.Postgres]:
    """Fresh server per test: use when the test needs a pristine instance."""
    yield from _server(pg_install_dir)


@pytest.fixture(scope="session")
def postgres_server_session(
    pg_install_dir: str, sw_log_dir: Path
) -> Iterator[sw.Postgres]:
    """One server for the whole session: use when createdb isolation is enough."""
    yield from _server(pg_install_dir)


@pytest.fixture
def sw_connect(
    postgres_server_session: sw.Postgres,
) -> Callable[..., sw.LoggedSQL]:
    """Connection factory; bound to postgres_server_session, not postgres_server."""

    def factory(dbname: str = "postgres") -> sw.LoggedSQL:
        # unique log name per connection: spdlog loggers are get-or-create per process
        return sw.connect_pg(
            log_name=f"pytest-{next(_log_ids)}",
            **postgres_server_session.connection_params(dbname),
        )

    return factory


def _detect_mysql_dir() -> str | None:
    env = os.environ.get("STORMWEAVER_MYSQL_DIR")
    if env:
        return env
    candidates = [
        Path.home() / ".local/share/stormweaver-mysql",
        Path("/usr"),
    ]
    for c in candidates:
        if (c / "bin" / "mysqld").exists() or (c / "sbin" / "mysqld").exists():
            return str(c)
    return None


@pytest.fixture(scope="session")
def mysql_install_dir() -> str:
    d = _detect_mysql_dir()
    if d is None:
        pytest.skip("no mysql installation (set STORMWEAVER_MYSQL_DIR)")
    return d


def _mysql_server(mysql_install_dir: str) -> Iterator[sw.MySQL]:
    datadir = tempfile.mkdtemp(prefix="sw-pytest-my-", dir="/tmp")
    my: sw.MySQL | None = None
    try:
        my = sw.MySQL(
            install_dir=mysql_install_dir,
            datadir=str(Path(datadir) / "data"),
            port=next(_ports),
        )
        my.start()
        assert my.wait_ready(), "mysql did not become ready"
        my.createdb("test")
        yield my
    finally:
        if my is not None:
            my.stop()
        shutil.rmtree(datadir, ignore_errors=True)


@pytest.fixture
def mysql_server(mysql_install_dir: str, sw_log_dir: Path) -> Iterator[sw.MySQL]:
    """Fresh server per test: use when the test needs a pristine instance."""
    yield from _mysql_server(mysql_install_dir)


@pytest.fixture(scope="session")
def mysql_server_session(
    mysql_install_dir: str, sw_log_dir: Path
) -> Iterator[sw.MySQL]:
    """One server for the whole session: use when createdb isolation is enough."""
    yield from _mysql_server(mysql_install_dir)


@pytest.fixture
def sw_connect_mysql(
    mysql_server_session: sw.MySQL,
) -> Callable[..., sw.LoggedSQL]:
    """Connection factory; bound to mysql_server_session, not mysql_server."""

    def factory(dbname: str = "test") -> sw.LoggedSQL:
        return sw.connect_mysql(
            log_name=f"pytest-my-{next(_log_ids)}",
            **mysql_server_session.connection_params(dbname),
        )

    return factory

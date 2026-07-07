import random

from stormweaver import log as swlog
from stormweaver.config import _bindable


def alloc_port(low: int = 26600, high: int = 27000) -> int:
    """Random free port; random start avoids collisions between parallel runs."""
    for _ in range(200):
        port = random.randrange(low, high)
        if _bindable(port):
            return port
    raise RuntimeError(f"no free port in range {low}-{high}")


def ensure_logging(name: str = "testing") -> None:
    """Idempotent run-log init: standalone runs have no fixture to do it."""
    if swlog.log_dir() is None:
        swlog.init_run_logging(name)


def pg_install_dir() -> str:
    # local import: pytest_plugin pulls in pytest, keep package import light
    from stormweaver.pytest_plugin import _detect_pg_dir

    d = _detect_pg_dir()
    if d is None:
        raise RuntimeError("no postgres installation found, set STORMWEAVER_PG_DIR")
    return d


def mysql_install_dir() -> str:
    from stormweaver.pytest_plugin import _detect_mysql_dir

    d = _detect_mysql_dir()
    if d is None:
        raise RuntimeError("no mysql installation found, set STORMWEAVER_MYSQL_DIR")
    return d

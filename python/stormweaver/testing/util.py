import os
from typing import Any

from stormweaver import log as swlog
from stormweaver.config import alloc_port as alloc_port


def ensure_logging(name: str = "testing", mode: str = "unified") -> None:
    """Idempotent run-log init: standalone runs have no fixture to do it."""
    if swlog.log_dir() is None:
        swlog.init_run_logging(name, mode=mode)


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


def keyring_params() -> list[Any]:
    """Parametrize values over keyring kinds; unavailable kinds skip-marked.

    With STORMWEAVER_KEYRINGS set only the forced kinds appear (selection
    already hard-errors on forced-but-unavailable kinds).
    """
    # local import: keep pytest out of plain `import stormweaver.testing`
    import pytest

    from stormweaver import keyrings

    selected = keyrings.selected_keyrings()
    if os.environ.get("STORMWEAVER_KEYRINGS"):
        return [pytest.param(k) for k in selected]
    return [
        pytest.param(k)
        if k in selected
        else pytest.param(
            k, marks=pytest.mark.skip(reason=f"keyring {k!r} not available")
        )
        for k in keyrings.KINDS
    ]


def require_managed(keyring: Any) -> None:
    """Skip the calling test unless stormweaver controls the keyring's service.

    Lifecycle-dependent tests (fresh-restart negative paths) call this so an
    external/serviceless keyring skips instead of failing.
    """
    import pytest

    if not keyring.managed:
        pytest.skip(f"keyring {keyring.kind!r} service not managed by stormweaver")

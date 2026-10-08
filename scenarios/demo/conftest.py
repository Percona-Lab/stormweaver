import pytest
from stormweaver.pytest_plugin import _detect_mysql_dir, _detect_pg_dir

PG_DIR = _detect_pg_dir()
MYSQL_DIR = _detect_mysql_dir()

requires_pg = pytest.mark.skipif(
    PG_DIR is None, reason="no postgres installation (set STORMWEAVER_PG_DIR)"
)
requires_mysql = pytest.mark.skipif(
    MYSQL_DIR is None, reason="no mysql installation (set STORMWEAVER_MYSQL_DIR)"
)

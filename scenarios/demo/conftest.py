import os
from pathlib import Path

import pytest
from stormweaver.pytest_plugin import _detect_mysql_dir, _detect_pg_dir

PG_DIR = _detect_pg_dir()
MYSQL_DIR = _detect_mysql_dir()
TDE_DIR = os.environ.get(
    "STORMWEAVER_PG_TDE_DIR", "/storage/tdework/inst/psp_rel_18_stable"
)

requires_pg = pytest.mark.skipif(
    PG_DIR is None, reason="no postgres installation (set STORMWEAVER_PG_DIR)"
)
requires_mysql = pytest.mark.skipif(
    MYSQL_DIR is None, reason="no mysql installation (set STORMWEAVER_MYSQL_DIR)"
)
requires_pg_tde = pytest.mark.skipif(
    not (Path(TDE_DIR) / "bin" / "initdb").exists(),
    reason="no pg_tde-enabled postgres (set STORMWEAVER_PG_TDE_DIR)",
)

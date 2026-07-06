import getpass
import os
import shutil
import tempfile
from contextlib import contextmanager
from pathlib import Path

import pytest
import stormweaver as sw

PG_DIR = Path(
    os.environ.get(
        "STORMWEAVER_PG_DIR", str(Path.home() / ".local/share/stormweaver-pg18")
    )
)

requires_pg = pytest.mark.skipif(
    not (PG_DIR / "bin" / "initdb").exists(), reason="no postgres installation"
)

PG_TDE_DIR = Path(
    os.environ.get("STORMWEAVER_PG_TDE_DIR", "/storage/tdework/inst/psp_rel_18_stable")
)

requires_pg_tde = pytest.mark.skipif(
    not (PG_TDE_DIR / "bin" / "initdb").exists(),
    reason="no pg_tde postgres installation",
)


def pg_user() -> str:
    return os.environ.get("PGUSER") or getpass.getuser()


@contextmanager
def running_pg(port: int, dbname: str):
    # tmp_path can blow the 107-byte unix socket limit; short /tmp dir instead
    datadir = tempfile.mkdtemp(dir="/tmp")
    pg = sw.Postgres(
        install_dir=str(PG_DIR), datadir=str(Path(datadir) / "data"), port=port
    )
    pg.start()
    try:
        assert pg.wait_ready(), "postgres did not become ready"
        pg.createdb(dbname)
        yield pg
    finally:
        pg.stop()
        shutil.rmtree(datadir, ignore_errors=True)

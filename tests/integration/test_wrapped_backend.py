import contextlib
import os
import shutil
import signal
import subprocess
import tempfile
from pathlib import Path

import pytest
import stormweaver as sw
from conftest import PG_DIR, requires_pg
from stormweaver import log as swlog
from stormweaver.wrappers import ExecPrefixWrapper, RRWrapper


def _rr_works() -> bool:
    if shutil.which("rr") is None:
        return False
    with tempfile.TemporaryDirectory() as tmp:
        result = subprocess.run(
            ["rr", "record", "true"],
            capture_output=True,
            env=os.environ | {"_RR_TRACE_DIR": tmp},
        )
    return result.returncode == 0


def require_rr():
    if not _rr_works():
        pytest.skip("rr cannot record on this system")


def make_pg(port, wrapper=None):
    # tmp_path can blow the 107-byte unix socket limit; short /tmp dir instead
    base = tempfile.mkdtemp(dir="/tmp")
    pg = sw.Postgres(
        install_dir=str(PG_DIR),
        datadir=str(Path(base) / "data"),
        port=port,
        wrapper=wrapper,
    )
    return pg, base


@requires_pg
def test_pg_lifecycle_under_exec_prefix(tmp_path, monkeypatch):
    monkeypatch.setattr(swlog, "_run_dir", tmp_path / "run")
    (tmp_path / "run").mkdir()
    pg, base = make_pg(26411, wrapper=ExecPrefixWrapper(["env"]))
    try:
        pg.start()
        assert pg.wait_ready(), "postgres did not become ready under wrapper"
        assert pg.is_running()
    finally:
        with contextlib.suppress(Exception):
            pg.stop()
        shutil.rmtree(base, ignore_errors=True)
    assert not pg.is_running()
    text = (tmp_path / "run" / "outcome").read_text()
    assert "result=clean" in text


@requires_pg
def test_unexpected_death_marks_crashed(tmp_path, monkeypatch):
    monkeypatch.setattr(swlog, "_run_dir", tmp_path / "run")
    (tmp_path / "run").mkdir()
    pg, base = make_pg(26412)
    try:
        pg.start()
        assert pg.wait_ready()
        pid = pg._server_pid()
        assert pid is not None
        os.kill(pid, signal.SIGKILL)
    finally:
        with contextlib.suppress(Exception):
            pg.stop()
        shutil.rmtree(base, ignore_errors=True)
    text = (tmp_path / "run" / "outcome").read_text()
    assert "result=crashed" in text
    assert "SIGKILL" in text


@requires_pg
def test_rr_deletes_clean_trace(tmp_path, monkeypatch):
    require_rr()
    monkeypatch.setattr(swlog, "_run_dir", tmp_path / "run")
    (tmp_path / "run").mkdir()
    pg, base = make_pg(26413, wrapper=RRWrapper())
    trace = tmp_path / "run" / "rr"
    try:
        pg.start()
        assert pg.wait_ready(), "postgres did not become ready under rr"
        assert any(trace.iterdir()), "rr trace dir not created"
    finally:
        with contextlib.suppress(Exception):
            pg.stop()
        shutil.rmtree(base, ignore_errors=True)
    assert not any(trace.iterdir()), "clean trace should have been deleted"
    assert "result=clean" in (tmp_path / "run" / "outcome").read_text()


@requires_pg
def test_rr_keep_all_keeps_trace(tmp_path, monkeypatch):
    require_rr()
    monkeypatch.setattr(swlog, "_run_dir", tmp_path / "run")
    (tmp_path / "run").mkdir()
    pg, base = make_pg(26414, wrapper=RRWrapper(keep_all=True))
    try:
        pg.start()
        assert pg.wait_ready()
    finally:
        with contextlib.suppress(Exception):
            pg.stop()
        shutil.rmtree(base, ignore_errors=True)
    assert any((tmp_path / "run" / "rr").iterdir()), "keep_all trace missing"

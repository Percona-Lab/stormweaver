import argparse
import os
from pathlib import Path

import pytest
import stormweaver as sw
from conftest import PG_DIR, PG_TDE_DIR, requires_pg, requires_pg_tde
from stormweaver import scenario
from stormweaver.entropy import db_files_entropy

MYSQL_DIR = Path(
    os.environ.get(
        "STORMWEAVER_MYSQL_DIR", str(Path.home() / ".local/share/stormweaver-mysql")
    )
)

requires_mysql = pytest.mark.skipif(
    not (MYSQL_DIR / "bin" / "mysqld").exists(), reason="no mysql installation"
)


def make_opts(tmp_path, *extra, install_dir=PG_DIR):
    cfg = tmp_path / "stormweaver.toml"
    cfg.write_text(f'[default]\ndatadir_root = "{tmp_path / "datadirs"}"\n')
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=str(cfg))
    parser.add_argument("--install-dir", default=str(install_dir))
    scenario.add_common_arguments(parser)
    args = parser.parse_args(list(extra))
    return scenario.finalize(args)


@requires_pg
def test_single_pg_runs_workload(tmp_path):
    opts = make_opts(tmp_path, "--duration", "2", "--workers", "2")
    with scenario.single_pg(opts, initial_tables=3) as ctx:
        assert ctx.pg.is_running()
        assert ctx.metadata.size() >= 3
        assert ctx.registry.has("checkpoint")
        assert not ctx.registry.has("create_partition")
        # the process-wide default registry must stay untouched
        assert sw.default_action_registry().has("create_partition")
        assert not sw.default_action_registry().has("checkpoint")
        ctx.workload.run()
        conn = ctx.connect("check")
        res = conn.execute("SELECT 1;")
        assert res.success()
    assert not ctx.pg.is_running()


@requires_pg
def test_single_pg_stops_on_exception(tmp_path):
    opts = make_opts(tmp_path, "--duration", "2")
    with (
        pytest.raises(ValueError, match="boom"),
        scenario.single_pg(opts, initial_tables=1) as ctx,
    ):
        raise ValueError("boom")
    assert not ctx.pg.is_running()


@requires_pg
def test_single_pg_archive_config(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    opts = make_opts(tmp_path, "--duration", "2")
    with scenario.single_pg(opts, archive=True, initial_tables=1) as ctx:
        conf = (
            tmp_path / "datadirs" / "datadir_primary" / "postgresql.conf"
        ).read_text()
        assert "archive_mode = 'on'" in conf
        assert "summarize_wal = 'on'" in conf
        assert (tmp_path / "archive").is_dir()
        w = ctx.make_worker("verification")
        w.calculate_database_checksums(str(tmp_path / "t.checksum"))
        assert (tmp_path / "t.checksum").exists()


@requires_pg_tde
def test_single_pg_tde_on_wal(tmp_path):
    opts = make_opts(
        tmp_path,
        "--duration",
        "2",
        "--workers",
        "2",
        "--tde",
        "on_wal",
        install_dir=str(PG_TDE_DIR),
    )
    with scenario.single_pg(opts, initial_tables=2) as ctx:
        conn = ctx.connect("tde-check")
        res = conn.execute("SHOW default_table_access_method;")
        assert res.success()
        assert res.rows()[0][0] == "tde_heap"
        res = conn.execute("SHOW pg_tde.wal_encrypt;")
        assert res.success()
        assert res.rows()[0][0] == "on"
        assert ctx.keyring.exists()
        ctx.workload.run()
        db_files_entropy(ctx.connect("entropy"), ctx.datadir)
    assert not ctx.pg.is_running()


@requires_mysql
def test_single_mysql_runs_workload(tmp_path):
    opts = make_opts(
        tmp_path, "--duration", "2", "--workers", "2", install_dir=MYSQL_DIR
    )
    with scenario.single_mysql(opts, initial_tables=2) as ctx:
        assert ctx.db.is_running()
        assert ctx.metadata.size() >= 2
        ctx.workload.run()
        conn = ctx.connect("check")
        res = conn.execute("SELECT 1;")
        assert res.success()
    assert not ctx.db.is_running()

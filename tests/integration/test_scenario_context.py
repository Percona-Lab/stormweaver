import argparse
import os
from pathlib import Path

import pytest
import stormweaver as sw
from conftest import PG_DIR, requires_pg
from stormweaver import scenario

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


@requires_pg
def test_single_pg_hooks(tmp_path):
    opts = make_opts(tmp_path, "--duration", "2", "--workers", "2")
    calls = []

    def db_init(conn):
        calls.append("db_init")
        r = conn.execute(
            "CREATE ACCESS METHOD heap2 TYPE TABLE HANDLER heap_tableam_handler;"
        )
        assert r.success(), r.error_message
        r = conn.execute("ALTER SYSTEM SET work_mem = '7MB';")
        assert r.success(), r.error_message

    with scenario.single_pg(
        opts, initial_tables=1, access_methods=["heap2", "heap"], db_init=db_init
    ) as ctx:
        assert calls == ["db_init"]
        conn = ctx.connect("c")
        assert conn.execute("SHOW default_table_access_method;").rows()[0][0] == "heap2"
        assert conn.execute("SHOW work_mem;").rows()[0][0] == "7MB"
        res = conn.execute(
            "SELECT count(*) FROM pg_tables WHERE schemaname = 'public';"
        )
        assert int(res.rows()[0][0]) > 0


@requires_pg
def test_single_pg_bad_access_method_is_loud(tmp_path):
    opts = make_opts(tmp_path, "--duration", "2", "--workers", "2")
    with (
        pytest.raises(RuntimeError, match="cannot set default access method"),
        scenario.single_pg(
            opts, initial_tables=1, access_methods=["nonexistent_am"]
        ) as ctx,
    ):
        ctx.connect("x")

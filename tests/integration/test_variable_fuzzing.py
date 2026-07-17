import argparse
import os
import time
from pathlib import Path

import pytest
from conftest import PG_DIR, requires_pg
from stormweaver import scenario, variables

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


def _toggle_successes(ctx, names):
    total = 0
    for stats in ctx.workload.worker_statistics():
        for name in names:
            st = stats.action_stats(name)
            if st is not None:
                total += st.success_count
    return total


@requires_pg
def test_pg_variable_fuzzing_workload(tmp_path):
    opts = make_opts(
        tmp_path,
        "--duration",
        "5",
        "--workers",
        "2",
        "--var-fuzz",
        "safe",
        "--seed",
        "42",
    )
    with scenario.single_pg(opts, initial_tables=2) as ctx:
        # default toggle weight (5) is tiny next to the DML pool's (~6800),
        # a 5s/2-worker window can roll zero hits by pure chance; raise the
        # weight here so the toggle path reliably fires without changing
        # the feature's real-world defaults
        ctx.registry.get("set_session_variable").weight = 500
        ctx.registry.get("reload_global_variable").weight = 500
        ctx.workload.run()
        assert (
            _toggle_successes(ctx, ("set_session_variable", "reload_global_variable"))
            > 0
        )
        conn = ctx.connect("check")
        assert conn.execute("SELECT 1;").success()


@requires_pg
def test_pg_startup_roll_applied_and_deterministic(tmp_path):
    expected = variables.preset("postgres").roll_startup(variables.startup_rng(42))
    again = variables.preset("postgres").roll_startup(variables.startup_rng(42))
    assert expected == again
    assert len(expected) > 0

    opts = make_opts(tmp_path, "--duration", "1", "--var-fuzz", "safe", "--seed", "42")
    with scenario.single_pg(opts, initial_tables=1) as ctx:
        conf = (Path(ctx.datadir) / "postgresql.conf").read_text()
        for name, value in expected.items():
            assert f"{name} = '{value}'" in conf


@requires_pg
def test_pg_reload_and_reset_auto_conf(tmp_path):
    opts = make_opts(tmp_path, "--duration", "1")
    with scenario.single_pg(opts, initial_tables=1) as ctx:
        conn = ctx.connect("reload-check")
        assert conn.execute(
            "ALTER SYSTEM SET checkpoint_completion_target = 0.5;"
        ).success()
        assert conn.execute("SELECT pg_reload_conf();").success()

        deadline = time.monotonic() + 10
        value = None
        while time.monotonic() < deadline:
            value = conn.execute("SHOW checkpoint_completion_target;").rows()[0][0]
            if value == "0.5":
                break
            time.sleep(0.2)
        assert value == "0.5"

        auto_conf = Path(ctx.datadir) / "postgresql.auto.conf"
        assert "checkpoint_completion_target" in auto_conf.read_text()

        ctx.db.stop()
        ctx.pg.reset_auto_conf()
        assert not auto_conf.exists()
        ctx.db.start()
        assert ctx.db.wait_ready()


@requires_mysql
def test_mysql_variable_fuzzing_workload(tmp_path):
    opts = make_opts(
        tmp_path,
        "--duration",
        "5",
        "--workers",
        "2",
        "--var-fuzz",
        "safe",
        "--seed",
        "42",
        install_dir=MYSQL_DIR,
    )
    with scenario.single_mysql(opts, initial_tables=2) as ctx:
        # same reasoning as the pg workload test: raise toggle weight so
        # the mechanism reliably fires in a short window
        ctx.registry.get("set_session_variable").weight = 500
        ctx.registry.get("set_global_variable").weight = 500
        ctx.workload.run()
        assert (
            _toggle_successes(ctx, ("set_session_variable", "set_global_variable")) > 0
        )
        conn = ctx.connect("check")
        assert conn.execute("SELECT 1;").success()


@requires_mysql
def test_mysql_set_global_visible_across_sessions(tmp_path):
    opts = make_opts(tmp_path, "--duration", "1", install_dir=MYSQL_DIR)
    with scenario.single_mysql(opts, initial_tables=1) as ctx:
        setter = ctx.connect("setter")
        assert setter.execute("SET GLOBAL innodb_max_dirty_pages_pct = 42;").success()
        reader = ctx.connect("reader")
        res = reader.execute("SELECT @@GLOBAL.innodb_max_dirty_pages_pct;")
        assert res.rows()[0][0].startswith("42")

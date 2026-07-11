import os
import subprocess
import sys

from stormweaver import events

SCENARIO = """
import os
from stormweaver.testing import PgTestNode

LOG_MODE = "unified"
{extra}

def main(args):
    with PgTestNode.fresh(os.environ["STORMWEAVER_PG_DIR"]) as node:
        node.safe_sql("CREATE TABLE t (a int)")
        node.safe_sql("INSERT INTO t VALUES (1), (2)")
        node.expect_error("already exists", "CREATE TABLE t (a int)")
        assert node.sql_value("SELECT count(*) FROM t") == "2"
    return 0
"""

FAILING = """
import os
from stormweaver.testing import PgTestNode

LOG_MODE = "unified"

def main(args):
    with PgTestNode.fresh(os.environ["STORMWEAVER_PG_DIR"]) as node:
        node.safe_sql("CREATE TABLE t (a int)")
        node.safe_sql("INSERT INTO t VALUES (1), (2)")
        node.sql_value("SELECT a FROM t")
    return 0
"""


def _run(tmp_path, pg_install_dir, source):
    scen = tmp_path / "scen.py"
    scen.write_text(source)
    env = os.environ | {"STORMWEAVER_PG_DIR": pg_install_dir}
    env.pop("STORMWEAVER_LOG_MODE", None)
    env.pop("STORMWEAVER_LOG_SPLITS", None)
    proc = subprocess.run(
        [sys.executable, "-m", "stormweaver", str(scen)],
        cwd=tmp_path,
        env=env,
        capture_output=True,
        text=True,
        timeout=120,
    )
    logs = tmp_path / "logs"
    assert logs.is_dir(), proc.stderr
    (run_dir,) = logs.iterdir()
    return proc, run_dir


def _parsed(run_dir):
    out = []
    for line in (run_dir / "main.log").read_text().splitlines():
        split = events.parse_line(line)
        if split is None:
            continue
        _, _, name, message = split
        out.append((name, message, events.parse_event(message)))
    return out


def test_unified_scenario_single_ordered_log(tmp_path, pg_install_dir):
    proc, run_dir = _run(tmp_path, pg_install_dir, SCENARIO.format(extra=""))
    assert proc.returncode == 0, proc.stderr
    lines = _parsed(run_dir)
    kinds = [ev[0] for _, _, ev in lines if ev]
    assert kinds[0] == "RUN"
    assert kinds.index("NODE") < kinds.index("ASSERT")
    asserts = [ev for _, _, ev in lines if ev and ev[0] == "ASSERT"]
    assert asserts and all(f["status"] == "pass" for _, f in asserts)
    # statement/result detail from the C++ connection loggers, inline
    assert any(
        name.startswith("sql-conn-") and message.startswith("Statement:")
        for name, message, _ in lines
    )
    assert not list(run_dir.glob("sql-conn-*.log"))
    assert "scenario result=passed" in (run_dir / "outcome").read_text()


def test_unified_with_splits_writes_both(tmp_path, pg_install_dir):
    proc, run_dir = _run(
        tmp_path, pg_install_dir, SCENARIO.format(extra="LOG_SPLITS = True")
    )
    assert proc.returncode == 0, proc.stderr
    assert list(run_dir.glob("sql-conn-*.log"))
    assert any(name.startswith("sql-conn-") for name, _, _ in _parsed(run_dir))


def test_failing_assertion_dumps_context(tmp_path, pg_install_dir):
    proc, run_dir = _run(tmp_path, pg_install_dir, FAILING)
    assert proc.returncode == 1, proc.stderr
    lines = _parsed(run_dir)
    fails = [
        ev
        for _, _, ev in lines
        if ev and ev[0] == "ASSERT" and ev[1]["status"] == "fail"
    ]
    assert fails and fails[0][1]["kind"] == "sql_value"
    dumps = {ev[1]["what"] for _, _, ev in lines if ev and ev[0] == "DUMP"}
    assert {"traceback", "result", "server-log-tail"} <= dumps
    assert "scenario result=failed" in (run_dir / "outcome").read_text()

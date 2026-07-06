"""Determinism check: same seed => identical per-worker SQL statement sequence.

Each run happens in its own subprocess. Reason: stormweaver's SQL/worker
loggers are registered by name in spdlog's process-wide registry, and a
logger already registered under a name is reused (see
sql_variant::LoggedSQL / Worker ctor) rather than re-opened. Two runs in one
process with the same worker names would silently make the second run keep
appending to the first run's log file instead of writing its own. A fresh
subprocess per run gives each one a clean registry, and init_logging pins
each run's log files to a private <run_dir>/logs directory.

WORKERS is pinned to 1, found the hard way (see git history / task notes):
- With workers=1, the sequence of SQL statements a worker sends is fully
  reproducible for a fixed seed: verified twice, 500-800+ statements each
  run, zero divergence.
- With workers>=2, replay diverges within the first ~25-50 statements per
  worker, and it's a real logic-level divergence, not just log noise: e.g.
  run1 issues "UPDATE foo49281904 ..." where run2 issues
  "UPDATE foo39841352 ...". Root cause: action::find_random_table()
  (core/src/action/helper.cpp) draws
  rand.random_number(0, metaCtx.size() - 1) against the *shared* Metadata
  object, and ddl.cpp's CreateTable checks metaCtx.size() against
  max_table_count before consuming further rand draws. metaCtx.size()
  changes concurrently as other workers create/drop tables, so the same
  RNG draw picks a different table (or skips/doesn't skip rand draws)
  depending on cross-worker timing. That timing is real wall-clock thread
  scheduling and is not reproducible, so multi-worker replay is only
  per-worker *decision* deterministic (same RNG stream), not sequence
  identical. Single worker sidesteps this since there is no concurrent
  mutator of the shared metadata.
"""

import argparse
import getpass
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile

import stormweaver as sw
from stormweaver.log import init_logging

SEED = 987654321
WORKERS = 1
DURATION_SECONDS = 10
INITIAL_TABLES = 3
# a healthy 10s run produces 500-800+ statements; far fewer means the worker
# died early (e.g. exhausted reconnects) even though the run exited cleanly
MIN_STATEMENTS = 50
# the workload is duration-cut, not count-cut, so the two runs legitimately
# stop at slightly different statement counts (observed 813 vs 831 on an idle
# machine, ~7% apart under load) - exact count equality was tried and failed
# on the first verification run. A worker dying mid-run is a very different
# magnitude (e.g. 60 vs 815), so cap the allowed count ratio instead.
MAX_COUNT_RATIO = 2.0
# hard cap per replay subprocess, generous vs the ~20s a healthy run takes
RUN_TIMEOUT_SECONDS = 600

# uniform log line: "2026-07-06 09:44:31,624 [INFO] sql-conn-setup: Statement: ..."
# only the timestamp varies run to run, strip just that prefix.
# only "Statement: ..." lines are the actual SQL sequence a worker chose to
# send (driven purely by our seeded ps_random). The interleaved error lines
# record whether the server *accepted* a statement - server-side diagnostics,
# not part of the sequence the worker chose, so they stay out of the
# comparison.
_STATEMENT_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2},\d{3} "
    r"(\[INFO\] sql-conn-[\w.-]+: Statement: .*)$"
)


def _statement_text(line: str) -> str | None:
    m = _STATEMENT_RE.match(line)
    return m.group(1) if m else None


def _worker_names() -> list[str]:
    return [f"worker-1-{i + 1}" for i in range(WORKERS)]


def _free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def run_once(run_dir: str, install_dir: str, port: int) -> None:
    """Run one seeded setup+workload in run_dir, logs land under run_dir/logs."""
    run_dir = os.path.abspath(run_dir)
    os.makedirs(run_dir, exist_ok=True)
    init_logging(os.path.join(run_dir, "logs"))

    datadir = os.path.join(run_dir, "dd")
    shutil.rmtree(datadir, ignore_errors=True)

    pg = sw.Postgres(install_dir=install_dir, datadir=datadir, port=port)
    # autovacuum triggers on wall-clock timing and can shift row placement
    # between runs, which would leak into ORDER BY random() row picks
    pg.add_config({"autovacuum": "off"})
    pg.start()
    try:
        pg.wait_ready()
        pg.createdb("testdb")

        metadata = sw.Metadata()
        registry = sw.default_action_registry()
        # same trim as basic.py, keeps things simple
        for action_name in ["create_partition", "drop_partition"]:
            if registry.has(action_name):
                registry.remove(action_name)

        action_config = sw.AllConfig()
        action_config.ddl.access_methods = ["heap"]

        user = os.environ.get("PGUSER") or getpass.getuser()

        def make_connection(log_name: str) -> sw.LoggedSQL:
            conn = sw.connect_pg(
                host="localhost",
                port=pg.port,
                dbname="testdb",
                user=user,
                log_name=log_name,
            )
            # DML statements embed "ORDER BY random() LIMIT n" which runs
            # on the *server's* per-backend PRNG. Left unseeded, different
            # rows get deleted/updated each run, table contents diverge,
            # and DDL outcomes that depend on content (index row size,
            # FK violations) diverge with them - DDL outcomes update the
            # shared metadata, so the worker's later table picks diverge
            # too (observed once at statement ~700 of a single-worker
            # run). Seeding the backend makes content evolution
            # reproducible.
            conn.execute("SELECT setseed(0.42)")
            return conn

        setup_params = sw.WorkloadParams()
        setup_params.seed = SEED
        setup_worker = sw.Worker(
            "setup", lambda: make_connection("setup"), setup_params, metadata
        )
        setup_worker.create_random_tables(INITIAL_TABLES)

        workload = sw.Workload(
            workers=WORKERS,
            duration=DURATION_SECONDS,
            repeat=1,
            registry=registry,
            metadata=metadata,
            node_factory=make_connection,
            action_config=action_config,
            seed=SEED,
        )
        workload.run()
    finally:
        pg.stop()


def _run_subprocess(run_dir: str, install_dir: str, port: int) -> None:
    try:
        result = subprocess.run(
            [
                sys.executable,
                os.path.abspath(__file__),
                "--single-run",
                run_dir,
                install_dir,
                str(port),
            ],
            capture_output=True,
            text=True,
            timeout=RUN_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired:
        print(
            f"determinism FAILED: single run in {run_dir} timed out "
            f"after {RUN_TIMEOUT_SECONDS}s",
            file=sys.stderr,
        )
        raise RuntimeError(f"single run in {run_dir} timed out") from None
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        raise RuntimeError(f"single run in {run_dir} failed, see output above")


def _read_statements(path: str) -> list[str]:
    lines = []
    with open(path) as f:
        for line in f:
            stmt = _statement_text(line)
            if stmt is not None:
                lines.append(stmt)
    return lines


def compare_runs(run1_dir: str, run2_dir: str) -> bool:
    ok = True
    # the setup worker only issues INITIAL_TABLES CREATEs, the 50-statement
    # floor is for the duration-based workload workers
    floors = {"setup": INITIAL_TABLES}
    for name in ["setup", *_worker_names()]:
        f1 = os.path.join(run1_dir, "logs", f"sql-conn-{name}.log")
        f2 = os.path.join(run2_dir, "logs", f"sql-conn-{name}.log")

        if not os.path.exists(f1) or not os.path.exists(f2):
            print(f"determinism FAILED: missing SQL log for '{name}' ({f1} / {f2})")
            ok = False
            continue

        lines1 = _read_statements(f1)
        lines2 = _read_statements(f2)

        floor = floors.get(name, MIN_STATEMENTS)
        if len(lines1) < floor or len(lines2) < floor:
            print(
                f"determinism FAILED: worker '{name}' produced too few "
                f"statements (run1={len(lines1)}, run2={len(lines2)}, "
                f"minimum {floor}) - the worker likely died mid-run"
            )
            ok = False
            continue

        # grossly different counts mean one run's worker stopped early, which
        # the prefix comparison alone would not catch; small differences are
        # just the wall-clock duration cut (see MAX_COUNT_RATIO above)
        if max(len(lines1), len(lines2)) > MAX_COUNT_RATIO * min(
            len(lines1), len(lines2)
        ):
            print(
                f"determinism FAILED: worker '{name}' statement counts differ "
                f"too much (run1={len(lines1)}, run2={len(lines2)}, "
                f"allowed ratio {MAX_COUNT_RATIO}) - one worker died mid-run"
            )
            ok = False
            continue

        common = min(len(lines1), len(lines2))
        diverged_at = None
        for i in range(common):
            if lines1[i] != lines2[i]:
                diverged_at = i
                break

        if diverged_at is not None:
            print(f"determinism FAILED: worker '{name}' diverges at line {diverged_at}")
            print(f"  run1: {lines1[diverged_at].rstrip()}")
            print(f"  run2: {lines2[diverged_at].rstrip()}")
            ok = False
        else:
            print(
                f"worker '{name}' OK: common prefix of {common} statements "
                f"identical (run1={len(lines1)}, run2={len(lines2)})"
            )

    return ok


def main(args: argparse.Namespace) -> int:
    config = sw.Config.load(args.config)
    install_dir = args.install_dir or config.pgroot
    if not install_dir:
        raise RuntimeError(
            "PostgreSQL install dir required: use -i or set pgroot in config"
        )

    base_dir = os.environ.get("SW_DET_DIR")
    cleanup = base_dir is None
    if base_dir is None:
        base_dir = tempfile.mkdtemp(prefix="sw-determinism-")

    run1_dir = os.path.join(base_dir, "run1")
    run2_dir = os.path.join(base_dir, "run2")

    try:
        _run_subprocess(run1_dir, install_dir, _free_port())
        _run_subprocess(run2_dir, install_dir, _free_port())

        ok = compare_runs(run1_dir, run2_dir)
    finally:
        if cleanup:
            shutil.rmtree(base_dir, ignore_errors=True)

    if ok:
        print("determinism OK")
        return 0

    print("determinism FAILED")
    return 1


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "--single-run":
    run_once(sys.argv[2], sys.argv[3], int(sys.argv[4]))
    sys.exit(0)

# Stable tests

Besides randomized stress testing, StormWeaver ships a pytest plugin (`stormweaver.pytest_plugin`, registered as the `pytest11` entry point `stormweaver` - auto-active whenever the package is installed) for writing deterministic, always-green product tests against a real PostgreSQL server. `tests/stable/test_stable_example.py` is the template; `task test:py` and CI run `tests/unit`, stable tests live under `tests/stable`.

## Fixtures

| Fixture | Scope | Use for |
| --- | --- | --- |
| `pg_install_dir` | session | The PostgreSQL install dir; auto-detected (`STORMWEAVER_PG_DIR` env var, then `~/.local/share/stormweaver-pg18`, `/usr/lib/postgresql18`, `/usr/lib/postgresql/*`), skips the test if none found |
| `postgres_server` | function | A fresh server + fresh data directory per test - use when a test needs a pristine instance (e.g. mutates cluster-wide state) |
| `postgres_server_session` | session | One server shared by the whole test session - use when per-test `createdb()` isolation is enough; much cheaper than a fresh cluster per test |
| `sw_connect` | function | Connection factory bound to `postgres_server_session`, with a unique per-connection SQL log name |

Prefer `postgres_server_session` + `sw_connect` unless the test genuinely needs its own server (e.g. testing restart/crash behavior).

## Example

```python
import stormweaver as sw

def test_create_insert_select(postgres_server_session, sw_connect):
    postgres_server_session.createdb("stabledb")
    conn = sw_connect("stabledb")
    r = conn.execute("CREATE TABLE t1 (id int primary key, v text)")
    assert r.success()
    r = conn.execute(
        "INSERT INTO t1 SELECT g, 'row-' || g FROM generate_series(1, 100) g"
    )
    assert r.success()
    assert r.affected_rows == 100

def test_seeded_single_worker_workload(postgres_server_session, sw_connect):
    postgres_server_session.createdb("workloaddb")
    metadata = sw.Metadata()

    setup_params = sw.WorkloadParams()
    setup_params.seed = 777
    setup = sw.Worker("stable-wl-setup", lambda: sw_connect("workloaddb"), setup_params, metadata)
    setup.create_random_tables(2)

    workload = sw.Workload(
        workers=1, duration=3, registry=sw.default_action_registry(),
        metadata=metadata, node_factory=lambda: sw_connect("workloaddb"),
        seed=777, worker_name_prefix="stable-wl-",
    )
    workload.run()
    assert sum(s.total_success_count() for s in workload.worker_statistics()) > 0
```

(trimmed from `tests/stable/test_stable_example.py`)

A single-worker seeded workload like this is fully deterministic (see [Determinism](determinism.md)), which is what makes it safe to assert on in a stable test - unlike a full randomized multi-worker run, it won't flake.

## Logging

pytest sessions default to unified logging: each session writes one `main.log` into its run directory under `logs/` (ini option `stormweaver_log_mode = split` reverts to per-connection files). The testing framework's `safe_sql`/`sql_value`/`expect_error`/`poll_until`/`wait_for_log` helpers write `ASSERT` events into that log - `status=pass` on success, `status=fail` with the expected/actual values on failure - and a failure is followed by `DUMP` blocks with the traceback, the offending result rows, and a server log tail. See [Unified logging](writing-scenarios.md#unified-logging) for the event grammar.

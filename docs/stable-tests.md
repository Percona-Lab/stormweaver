# Stable tests

Besides randomized stress testing, StormWeaver ships a pytest plugin (`stormweaver.pytest_plugin`, registered as the `pytest11` entry point `stormweaver` - auto-active whenever the package is installed) for writing deterministic, always-green product tests against a real PostgreSQL server. `tests/stable/test_stable_example.py` is the template; `task test:py` and CI run `tests/unit`, stable tests live under `tests/stable`.

## Fixtures

| Fixture | Scope | Use for |
| --- | --- | --- |
| `pg_install` | session | A `PgInstall` describing the PostgreSQL under test, with `install_dir`, `bindir`, `pkglibdir`, `sharedir` and `major` (the last three are `None` when only a plain directory is known). Skips the test if no installation is found |
| `pg_install_dir` | session | The install dir as a string (`pg_install.install_dir`). Resolution order: `--pg-config`, `STORMWEAVER_PG_CONFIG`, `STORMWEAVER_PG_DIR`, then known directories (`~/.local/share/stormweaver-pg*`, `/usr/lib/postgresql*`, `/usr/lib/postgresql/*`, `/usr/pgsql-*`; newest first). A `pg_config` that cannot be run is an error, not a skip; only "nothing found" skips |
| `postgres_server` | function | A fresh server + fresh data directory per test - use when a test needs a pristine instance (e.g. mutates cluster-wide state) |
| `postgres_server_session` | session | One server shared by the whole test session - use when per-test `createdb()` isolation is enough; much cheaper than a fresh cluster per test |
| `sw_connect` | function | Connection factory bound to `postgres_server_session`, with a unique per-connection SQL log name |
| `sw_log_dir` | session | The run's log directory (see [Logging](#logging)); the server fixtures depend on it, so logging is initialised before the first server starts |

MySQL has the same shape: `mysql_install_dir` (`STORMWEAVER_MYSQL_DIR`, then `~/.local/share/stormweaver-mysql`, then `/usr`), `mysql_server`, `mysql_server_session` and `sw_connect_mysql`.

Prefer `postgres_server_session` + `sw_connect` unless the test genuinely needs its own server (e.g. testing restart/crash behavior).

## Running

```
stormweaver test tests/ --pg-config /path/to/install/bin/pg_config
```

`stormweaver test` runs pytest from StormWeaver's own interpreter and passes every argument through, so `-k`, `-x`, `-q`, paths and node ids all work as usual. It needs pytest; for an install without the dev group, use `pip install 'stormweaver[test]'`. Plain `pytest` works too, as the plugin is auto-registered.

Options added by the plugin:

* `--pg-config PATH` - the `pg_config` of the installation under test. Falls back to `STORMWEAVER_PG_CONFIG`, `STORMWEAVER_PG_DIR` and the known directories (see the fixture table).
* `--matrix NAME=a,b` - run tests that use matrix dimension `NAME` with these values. Repeatable, but only one spec per dimension.
* `--full` - every registered dimension, at every available value.

`pg_config` reports its own location, so for a meson or other out-of-tree build pass the `pg_config` from the installed tree (under the install prefix), not one from the build directory.

## Matrix dimensions

A dimension is an opt-in parameter that a test requests by fixture name. By default it has a single value, so a normal run is not multiplied. Register it in a conftest:

```python
from stormweaver.matrix import register_dimension

def pytest_configure(config):
    register_dimension(config, "flavor", default="plain",
                       available=lambda: ["plain", "spicy", "sweet"])
```

```python
def test_uses(flavor):
    ...
```

Test ids are `name-value`, e.g. `test_uses[flavor-spicy]`, so `-k flavor-spicy` selects one value. The default run uses only `default`; `--matrix flavor=spicy,sweet` runs exactly those; `--full` runs all of `available()`.

* Registration has to happen in a root or initial conftest (`pytest_configure`), because `--matrix` is validated at session start. An unknown dimension, an unknown value, a malformed spec, or the same dimension given twice is a usage error. Duplicate values within a spec are dropped.
* `available()` is called lazily, at most once per session, and never in the default run - it can be expensive (probing installs, listing providers).
* A test that parametrizes the same name itself with `@pytest.mark.parametrize` keeps its own values; the dimension is left alone for that test.

## Temporary directories

Each session gets a private `--basetemp` under `/tmp/sw-pytest-tmp-*`, so concurrent sessions (e.g. meson running test files in parallel) cannot prune each other's `tmp_path`. It is removed when the session passes and kept when anything fails, for inspection. An explicit `--basetemp` is honoured and never removed.

## Testing helpers

`stormweaver.testing` is the TAP-style toolkit the fixtures above are built on; product test suites (pg_tde's is the first) use it directly:

* `PgTestNode.fresh(install_dir, name=, config=, initdb_args=, ...)` - a started server with `safe_sql`, `sql_value`, `expect_error`, `poll_until`, `wait_for_log`, `kill`/`poll_start`, `basebackup(dest, extra_args, tablespace_map)` (`--checkpoint=fast` unless you pass a checkpoint option), `from_backup`, `wait_for_catchup`, `psql(sql, user=)`, `pg_major`.
* `RewindDriver` - the pg_rewind test cycle (setup, standby, promote, rewind in local/remote/archive mode). Subclasses override `primary_config`, `initdb_args`, `configure_primary`, `rewind_user`, `node_cls` and `backup_primary()` to swap the node class or the backup tool.
* Frontend tool checks: `program_help_ok`, `program_version_ok`, `program_options_handling_ok` (same semantics as PostgreSQL::Test::Utils), `run_bin`, `command_ok`, `command_fails(cp, stderr_re=)`.
* Files: `wal_strings(pg_wal_dir)` (like `strings` over the WAL segments), `corrupt_page_checksum(path, block)`, `check_mode_recursive(path, dir_mode, file_mode)`, `read_file_bytes`.
* Keyring services: `stormweaver.keyrings.open_keyring(kind, workdir)` provisions OpenBao or Cosmian KMS (executable, container or external, see `config/stormweaver.toml`); `VaultKeyring.cli(*args)` runs the `bao` CLI against a provisioned dev server with the right environment; `keyring.restart_service(fresh=True)` wipes and restarts it.
* Ports: `alloc_port()` picks a free port and reserves it for the process with a lock file under `/tmp/stormweaver-ports` (`STORMWEAVER_PORT_LOCK_DIR`), so parallel sessions on one host do not collide, also while a server is stopped.
* `Postgres.set_signal()` writes `standby.signal`/`recovery.signal` with group read access when the data directory was created with `initdb -g`.

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

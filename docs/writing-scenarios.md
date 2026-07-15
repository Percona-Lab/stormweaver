# Writing scenarios

A scenario is a plain Python file with a `main(args)` function. `stormweaver <scenario.py> [-c config.toml] [-i /pg/install/dir]` loads the file and calls `main(args)` with the parsed CLI namespace (`args.config`, `args.install_dir`, plus any scenario-specific options). A scenario declares its own options with an optional `add_arguments(parser)` function: stormweaver imports the scenario, folds those options into its own argument parser, and only then parses the command line - so `stormweaver <scenario.py> --help` lists the scenario's options too, and a mistyped flag is rejected instead of silently ignored.

Almost every scenario should start from `stormweaver.scenario`: `scenario.add_common_arguments()`/`scenario.finalize()` for the common CLI options, and `scenario.single_pg()`/`scenario.single_mysql()` for a ready-to-run server + workload. This page documents that framework. For raw access to the underlying pieces (`sw.Postgres`, `sw.Workload`, `sw.Worker`, ...), see [Randomized testing concepts](randomized-testing-concepts.md); the framework is built directly on top of them.

## Running a scenario

```bash
stormweaver <scenario.py> -c <config> -i <install dir> [common options] [scenario options]
```

* `-c/--config` - TOML config file, default `config/stormweaver.toml` (see [Config parameters](config-parameters.md))
* `-i/--install-dir` - database installation directory; falls back to `pgroot` in the config file
* `-v/--verbose` / `-q/--quiet` - console verbosity (debug / warnings only)
* `--log-mode`/`--log-splits` - log layout, see [Unified logging](#unified-logging)
* everything else is scenario-specific, declared by the scenario's `add_arguments` (see below)

A run creates, relative to the current working directory: `datadirs/` (one subdirectory per server, via `Config.datadir()`) and `logs/<timestamp>-<scenario name>/`. In the default split mode that run directory holds the main log plus per-server, per-worker and per-connection SQL logs; in unified mode everything goes into a single `main.log` instead. Backup-testing scenarios additionally create `backups/` and `archive/` (see below).

**Caveat: don't run from a deeply nested CWD.** PostgreSQL puts its Unix domain socket in the data directory (`unix_socket_directories` points at `datadirs/datadir_<name>`), and the kernel's socket path limit is about 107 bytes. A long enough working directory path pushes the socket path over that limit and `pg_ctl start` fails with no obviously-related error. Run from somewhere shallow (e.g. the repo root).

## Unified logging

Unified mode routes everything - scenario messages, server lifecycle, per-connection SQL statements - into one chronological `main.log`. Enable it with any of (highest precedence first): the `--log-mode unified` flag, `STORMWEAVER_LOG_MODE=unified` in the environment, or a `LOG_MODE = "unified"` module-level attribute in the scenario file. To also keep the split per-connection/worker files, add `--log-splits`, `STORMWEAVER_LOG_SPLITS=1`, or `LOG_SPLITS = True`. `-q` only quiets the console: the unified `main.log` always keeps full INFO detail.

Notable moments appear as structured event lines of the shape `KIND key=value key="quoted value"`. The kinds are `RUN` (run header: scenario, argv, cwd, `STORMWEAVER_*` env), `STEP` (script phase begin/end), `ASSERT` (a check, `status=pass`/`status=fail` with expected/actual fields), `NODE` (server lifecycle: init/start/stop/kill/promote), `DUMP` (multi-line context such as a traceback or server log tail - the block lines that follow are prefixed with `| `), and `OUTCOME` (final result). Mark your own script phases with `stormweaver.events.step`:

```python
from stormweaver import events

with events.step("restore from backup"):
    ...
```

## `add_arguments` / `scenario.add_common_arguments` / `scenario.finalize`

Options are declared in `add_arguments(parser)` (module-level, called by the CLI before parsing) and consumed in `main(args)` (called with the already-parsed namespace):

```python
def add_arguments(parser):
    scenario.add_common_arguments(parser)

def main(args):
    opts = scenario.finalize(args)
    ...
```

`scenario.add_common_arguments(parser)` registers the options every scenario shares:

| Option | Default | Meaning |
| --- | --- | --- |
| `--duration` | 10 | seconds per workload cycle |
| `--workers` | 5 | concurrent workers per cycle |
| `--repeat` | 5 | number of cycles the scenario should run (the scenario's own loop, `single_pg`/`single_mysql` don't loop for you) |
| `--tde` | `off` | `on` (per-database keys), `on_wal` (global keys + WAL encryption), or `off` |
| `--pgsm` | `off` | preload `pg_stat_monitor` |
| `--clear-logs` | off | delete old `logs/` subdirectories (never the current run's) |

`scenario.finalize(args)` does the post-parse work: resolves `args.config` (`Config.load(args.config)`) and `args.install_dir` (`args.install_dir` or `config.pgroot`, raises if neither is set), builds the wrapper object, and clears old logs if asked. It returns the same namespace, ready to hand to `single_pg`/`single_mysql`.

Add scenario-specific flags in `add_arguments` with `parser.add_argument(...)`, or override a common default with `parser.set_defaults(...)`:

```python
def add_arguments(parser):
    scenario.add_common_arguments(parser)
    parser.set_defaults(duration=30, workers=4, repeat=2)
```

`scenarios/demo/basic.py` uses `set_defaults` to change the default `tde` value; `scenarios/demo/test_pg_rewind_tde.py` shows a scenario that declares its own `--mode`/`--cipher`/`--keyring` and skips the common options entirely. A scenario with no options (e.g. `scenarios/ci/determinism.py`) omits `add_arguments` and just reads `args.config`/`args.install_dir` directly.

## `single_pg` / `single_mysql`

```python
with scenario.single_pg(opts, **kwargs) as ctx:
    ...
```

Sets up a fresh single server: wipes and recreates its data directory, starts it with sane defaults (`log_min_messages`, `max_connections`, `shared_buffers`, and `shared_preload_libraries` for `--tde`/`--pgsm`), waits for it to be ready, creates the test database, builds an action registry with the framework's standard actions (`checkpoint`, `vacuum_full_table`, `truncate_table`, `reindex_table`; partition actions removed - see `STANDARD_ACTIONS` in `scenario.py`), seeds it with initial tables, and returns a context with a workload ready to run. The server is stopped on exit even if the `with` block raises.

`single_pg(opts, *, archive=False, extra_config=None, conn_settings=None, db_setup=None, initial_tables=5, dbname="testdb", worker_setup=None, datadir_name="primary")`:

* `archive` - turn on WAL archiving/summarization (`archive_mode`, `archive_command` into `archive/`, `summarize_wal`) and create `archive/`; needed for `pg_basebackup`/PITR-style scenarios
* `extra_config` - dict merged into the server config, applied after the framework's own settings (can override them)
* `conn_settings` - callable `(conn) -> None`, run on every connection `ctx.connect()` opens, after the TDE access-method `SET`
* `db_setup` - callable `(worker) -> None`, replaces the default `worker.create_random_tables(initial_tables)` seeding step
* `initial_tables` - table count for the default seeding (ignored if `db_setup` is given)
* `dbname` - test database name
* `worker_setup` - callable `(worker, index) -> None`, forwarded to `Workload` (customize a specific worker's action registry; see `scenarios/demo/basic.py`)
* `datadir_name` - suffix for `Config.datadir()`, change it if a scenario runs more than one primary

If `opts.tde != "off"`, `single_pg` also creates a keyring file next to the data directory and runs `init_tde_only_for_db`/`init_tde_globally` before seeding tables. Either way, `single_pg` restarts the server once after seeding (TDE init may use `ALTER SYSTEM`, which needs a restart to take effect) - the context is only handed to the caller after that restart succeeds.

`single_mysql(opts, *, extra_config=None, db_setup=None, initial_tables=5, dbname="testdb", worker_setup=None, datadir_name="primary_mysql")` - same shape, no `archive`/TDE (not supported for MySQL yet).

Both are context managers yielding a `ScenarioContext` (`PgContext`/`MySqlContext`) with:

* `ctx.db` - the backend instance (`Postgres`/`MySQL`); `ctx.pg`/`ctx.my` is the same object under a backend-specific name
* `ctx.dbname`, `ctx.datadir` - the test database name and the server's data directory (as a string)
* `ctx.keyring` - keyring path (`PgContext` only, `None` if TDE is off)
* `ctx.metadata` - the shared `sw.Metadata()` for this server
* `ctx.registry` - the action registry `ctx.workload` was built from
* `ctx.workload` - a ready `sw.Workload` (`--workers`/`--duration` from `opts`); **one cycle per `.run()`/`.start()`+`.wait()` call** - scenarios own the `--repeat` loop themselves (see the example below). `.run()` blocks for the cycle; `.start()`/`.wait()` split that in two, with `.workers` exposing the live workers in between - use that to mutate a specific worker's action registry mid-run (see `scenarios/demo/basic.py`). `.print_report()`/`.worker_statistics()` cover the last `.run()` call (each `.run()` resets them); with the `.start()`/`.wait()` pattern they accumulate across cycles.
* `ctx.connect(log_name="scenario")` - open a fresh connection the same way the workload does (TDE access method set, `conn_settings` applied)
* `ctx.make_worker(name)` - a one-off `sw.Worker` with a unique name (for setup/verification/one-shot SQL outside the workload)
* `ctx.restart_and_wait(timeout=10)` - `ctx.db.restart(timeout)` then `wait_ready()`, raises if the server doesn't come back
* `ctx.validate_metadata_or_warn()` - runs `Worker.validate_metadata()`, logs a warning instead of failing (metadata can legitimately diverge under concurrent DDL - see [Determinism](determinism.md))

## Helpers

* `scenario.fresh_dir(*paths)` - delete and recreate each path (warns if it existed); use for scenario-owned directories like `backups/`/`archive/` before reusing them across runs
* `scenario.wait_for_log(path, pattern, timeout, offset=0)` - poll a log file for a substring, `True`/`False` on timeout; `offset` skips old content (e.g. from a previous restart of the same log file)
* `scenario.compare_checksums(actual, expected, what)` - compare two checksum files (as produced by `Worker.calculate_database_checksums()`), raise `RuntimeError` with a unified diff on mismatch

## Minimal example

The full contents of `scenarios/ci/basic.py`:

```python
# Basic CI scenario: randomized workload cycles against a single postgres
# primary with per-cycle restarts.

import logging

from stormweaver import scenario

logger = logging.getLogger("scenario.basic")


def add_arguments(parser):
    scenario.add_common_arguments(parser)
    parser.set_defaults(duration=30, workers=4, repeat=2)


def main(args):
    opts = scenario.finalize(args)

    with scenario.single_pg(opts) as ctx:
        for cycle in range(opts.repeat):
            ctx.workload.run()
            ctx.restart_and_wait()
            logger.info("cycle %d/%d done", cycle + 1, opts.repeat)

        ctx.validate_metadata_or_warn()

    print("Scenario completed successfully")
```

Run it with `stormweaver scenarios/ci/basic.py -i /path/to/pg/install`.

For a guided tour of the rest of the feature set - custom actions, per-worker registries, mid-run registry changes, restarts, pg_tde encryption verification, all with explanatory comments - read `scenarios/demo/basic.py` (needs a pg_tde-enabled build; run with `--workers 4 --tde on`).

## Backup-testing patterns

`scenarios/ci/pitr.py` and `scenarios/ci/incremental.py` share a shape worth knowing before writing another backup scenario:

1. `scenario.fresh_dir("backups", "archive")` up front
2. `single_pg(opts, archive=True)`, then `ctx.pg.basebackup(...)` for the base backup (`extra_args=["-c", "fast"]` to skip the checkpoint wait)
3. run workload cycles, and after each one that needs verifying, checksum with `ctx.make_worker(...).calculate_database_checksums(path)`
4. restore: stop the server (`ctx.pg.stop()`), replace the data directory (`shutil.rmtree` + `shutil.copytree`, or `ctx.pg.combinebackup(chain, ctx.datadir)` for incremental chains), start it again, and for PITR wait for the recovery pause with `scenario.wait_for_log(...)` before calling `pg_wal_replay_resume()`
5. after restore: a fresh worker's `reset_metadata()` + `discover_existing_schema()` re-syncs in-memory metadata with the restored schema, then checksum again and `scenario.compare_checksums(restored, expected, what)`

`pitr.py` additionally copies `archive/` to `archive-copy/` before the first restore, because recovery writes new timeline history into the archive and would poison it for the next restore iteration.

## Going fully manual

`single_pg`/`single_mysql` cover a fixed server topology (one server, standard actions, one workload). Reach for the underlying pieces directly - `sw.Postgres`, `sw.Workload`, `sw.Worker`, `stormweaver.log.init_logging` - when a scenario doesn't fit that shape: multiple independent servers/processes, non-standard directory layout, or comparing across subprocess runs.

`scenarios/ci/determinism.py` is the example: it runs two seeded workloads in separate subprocesses (each needs its own spdlog logger registry - see the comment at the top of that file for why) and diffs their SQL logs. It builds its own `Postgres`/`Workload`/`Worker` instead of going through `scenario.finalize`/`single_pg`, because the subprocess boundary and log comparison logic don't fit the framework's assumptions.

## Further reading

* [Randomized testing concepts](randomized-testing-concepts.md) - `Workload`/`Worker`/`Action`/`ActionRegistry`, the pieces `scenario.py` builds on
* [Python actions](python-actions.md) - registering custom actions
* [Determinism](determinism.md) - what replays and what doesn't, and the metadata-validation limitation `validate_metadata_or_warn()` works around
* [Config parameters](config-parameters.md) - the TOML config file and CLI flags

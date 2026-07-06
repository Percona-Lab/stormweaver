# Writing scenarios

A scenario is a plain Python file with a `main(args)` function. `stormweaver <scenario.py> [-c config.toml] [-i /pg/install/dir]` loads the file and calls `main(args)` with the parsed CLI namespace (`args.config`, `args.install_dir`, plus scenario-specific args if you extend `argparse`).

## Anatomy

```python
import stormweaver as sw

def main(args):
    config = sw.Config.load(args.config)
    install_dir = args.install_dir or config.pgroot

    datadir = config.datadir("primary")
    pg = sw.Postgres(install_dir=install_dir, datadir=datadir, port=config.port_start)
    pg.add_config({"max_connections": "100", "shared_buffers": "128MB"})
    pg.start()
    try:
        pg.wait_ready()
        pg.createdb("testdb")

        metadata = sw.Metadata()
        registry = sw.default_action_registry()

        def make_connection():
            return sw.connect_pg(host="localhost", port=pg.port, dbname="testdb", user="postgres")

        # seed the schema before the randomized workload starts
        worker = sw.Worker("setup", make_connection, sw.WorkloadParams(), metadata)
        worker.create_random_tables(5)

        workload = sw.Workload(
            workers=4,
            duration=30,
            repeat=2,
            registry=registry,
            metadata=metadata,
            node_factory=make_connection,
            seed=1234,
        )
        workload.run()
        workload.print_report()
    finally:
        pg.stop()
```

(trimmed from `scenarios/ci/basic.py` - see that file for the full, runnable version, including trimming partition actions and validating metadata after the run)

## Backend lifecycle

`sw.Postgres(install_dir, datadir, port=...)` runs `initdb` on construction. `add_config()` appends to `postgresql.conf` before `start()`. `start()`/`stop()`/`restart()`/`kill()` control the server process; `wait_ready()` polls `pg_isready`; `createdb()`/`dropdb()` manage databases. Always `pg.stop()` in a `finally` block.

## Workload

`sw.Workload(workers, duration, registry, metadata, node_factory, repeat=1, max_reconnect_attempts=5, action_config=None, seed=0, worker_name_prefix="")`:

* `workers` - number of concurrent worker threads per cycle
* `duration` - seconds each cycle runs (wall-clock cut, not statement-count cut)
* `repeat` - number of cycles; `.run()` blocks until all cycles finish
* `registry` - an `ActionRegistry` (start from `sw.default_action_registry()`, add/remove actions with `.register_python()`/`.remove()`/`.has()`)
* `metadata` - a shared `sw.Metadata()`, mutated by every worker as it creates/drops objects
* `node_factory` - zero-arg or one-arg (worker name) callable returning a fresh connection; one-arg form gets a per-worker SQL log file
* `action_config` - an `sw.AllConfig()` (`.ddl.*`, `.dml.*`) to tune what actions can do, e.g. restrict DDL to `access_methods = ["heap"]`
* `worker_name_prefix` - required if you run more than one `Workload` in the same process (worker names double as spdlog logger names, which are get-or-create per process - a clash silently makes two workloads append to the same log file)

`workload.print_report()` prints each worker's statistics; `workload.worker_statistics()` returns the `WorkerStatistics` objects for programmatic checks.

## Seeds

`seed=` on `Workload` (or `WorkloadParams` for a standalone `Worker`) seeds a per-worker RNG stream, derived deterministically from the seed and the worker's name. See [Determinism](determinism.md) for exactly what that does and doesn't guarantee.

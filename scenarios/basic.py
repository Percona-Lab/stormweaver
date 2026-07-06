# Basic demo scenario: what stormweaver can do, with explanatory comments.
#
# A scenario is a plain python file with a main(args) function; run it with:
#   stormweaver scenarios/basic.py -i /path/to/pg-with-pg_tde [--repeat N]
#
# This demo needs a PostgreSQL build that includes pg_tde (Percona Server
# for PostgreSQL) - it encrypts tables and verifies the files on disk.

import argparse
import getpass
import logging
import os
import shutil
import time
from pathlib import Path

import stormweaver as sw
from stormweaver.entropy import db_files_entropy
from stormweaver.tde import init_tde_only_for_db

logger = logging.getLogger("scenario.basic")


def main(args):
    # Scenarios can define their own arguments: anything the stormweaver CLI
    # doesn't recognize is passed through in args.extra.
    extra = argparse.ArgumentParser(prog="basic.py")
    extra.add_argument("--repeat", type=int, default=5, help="workload cycles")
    opts = extra.parse_args(args.extra)

    config = sw.Config.load(args.config)
    install_dir = args.install_dir or config.pgroot
    if not install_dir:
        raise RuntimeError(
            "PostgreSQL install dir required: use -i or set pgroot in config"
        )

    # Fresh data directory and keyring on every run - pg_tde keys must match
    # the cluster they were created for.
    datadir = config.datadir("primary")
    shutil.rmtree(datadir, ignore_errors=True)
    # postgres chdir's into the data directory on startup, so a relative
    # keyring path would be resolved from there - use an absolute path.
    keyring = Path(datadir).resolve().parent / "basic_keyring.per"
    keyring.parent.mkdir(parents=True, exist_ok=True)
    keyring.unlink(missing_ok=True)

    pg = sw.Postgres(install_dir=install_dir, datadir=datadir, port=config.free_port())
    pg.add_config(
        {
            # pg_tde must be preloaded before any encrypted table is touched
            "shared_preload_libraries": "pg_tde",
            "log_min_messages": "warning",
            "max_connections": "100",
            "shared_buffers": "256MB",
        }
    )
    pg.start()
    try:
        if not pg.wait_ready():
            raise RuntimeError("postgres did not become ready in time")
        pg.createdb("testdb")

        user = os.environ.get("PGUSER") or getpass.getuser()

        # Every connection goes through this factory. Per-connection session
        # settings belong here - the old lua demo called this conn_settings.
        def make_connection(log_name="setup"):
            conn = sw.connect_pg(
                host="localhost",
                port=pg.port,
                dbname="testdb",
                user=user,
                log_name=log_name,
            )
            conn.execute("SET default_table_access_method = tde_heap;")
            return conn

        # One-time database setup: extension, file key provider, principal
        # key. After this, tde_heap tables in testdb are encrypted.
        init_tde_only_for_db(make_connection("admin"), str(keyring))

        # Actions (mostly SQL statement generators) live in registries.
        # default_action_registry() returns THE process-wide default -
        # modifying it here affects everything created from it later.
        default_registry = sw.default_action_registry()

        # Partition actions are trimmed for demo stability, same as the CI
        # scenario.
        for name in ("create_partition", "drop_partition"):
            if default_registry.has(name):
                default_registry.remove(name)

        # Custom SQL actions: {table} is substituted with a random known
        # table. The number is the action's weight (relative pick frequency).
        default_registry.make_custom_table_sql(
            "vacuum_full_table", "VACUUM FULL {table};", 2
        )

        # Registries COPY on use(): node_registry snapshots the default as it
        # is right now. Later changes to either side don't affect the other.
        node_registry = sw.ActionRegistry()
        node_registry.use(default_registry)

        # Added to the default AFTER the snapshot: node_registry has no
        # truncate_table...
        default_registry.make_custom_table_sql("truncate_table", "TRUNCATE {table};", 2)
        # ...and the default has no reindex.
        node_registry.make_custom_table_sql("reindex", "REINDEX TABLE {table};", 1)

        # Metadata tracks the tables workers create/alter/drop; it is shared
        # by every worker in the workload.
        metadata = sw.Metadata()

        # Seed the schema before the randomized workload starts.
        setup_worker = sw.Worker(
            "setup", lambda: make_connection("setup"), sw.WorkloadParams(), metadata
        )
        setup_worker.create_random_tables(5)

        # action_config tunes what generated DDL may do; mixing access
        # methods means the entropy check below sees both encrypted and
        # plain tables.
        action_config = sw.AllConfig()
        action_config.ddl.access_methods = ["tde_heap", "heap"]

        # Each worker copies the workload's registry at construction;
        # worker_setup then customizes individual workers. Indices are
        # 0-based. Runs again for every cycle (workers are per-cycle).
        def worker_setup(worker, idx):
            actions = worker.possible_actions()
            if idx == 1:
                # worker 1 swaps to the CURRENT default registry: it gets
                # truncate_table, but loses reindex
                actions.use(default_registry)
            elif idx == 2:
                # crank up alter_table on worker 2
                actions.get("alter_table").weight = 127
            elif idx == 3:
                # worker 3 gets an extra action; not table based
                actions.make_custom_sql("checkpoint", "CHECKPOINT;", 1)

        workload = sw.Workload(
            workers=5,
            duration=10,
            registry=node_registry,
            metadata=metadata,
            node_factory=make_connection,
            action_config=action_config,
            worker_setup=worker_setup,
        )

        for cycle in range(opts.repeat):
            # start() launches the worker threads and returns; the demo uses
            # the running window to modify a worker mid-flight
            workload.start()
            time.sleep(1)
            if cycle == 0:
                # one second in, stop worker 2 from running alter_table at
                # all - registries can be modified while the worker runs
                workload.workers[2].possible_actions().remove("alter_table")
            workload.wait()

            # Restart the server; next cycle's workers connect fresh, and
            # mid-cycle restarts would be survived too (workers reconnect).
            pg.restart()
            if not pg.wait_ready():
                raise RuntimeError("postgres did not become ready after restart")

            # Verify encryption on disk: every tde_heap relation file must
            # be encrypted, everything else must not be.
            db_files_entropy(make_connection("verification"), datadir)
            logger.info("cycle %d/%d verified", cycle + 1, opts.repeat)

        workload.print_report()
    finally:
        pg.stop()

    print("Demo scenario completed successfully")

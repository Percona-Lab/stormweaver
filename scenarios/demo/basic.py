# Basic demo scenario: what stormweaver can do, with explanatory comments.
#
# A scenario is a plain python file with a main(args) function; run it with:
#   stormweaver scenarios/basic.py -i /path/to/pg-with-pg_tde [--repeat N]
#
# This demo needs a PostgreSQL build that includes pg_tde (Percona Server
# for PostgreSQL) - it encrypts tables and verifies the files on disk.
# Common options (--duration/--workers/--repeat/--tde) apply; this demo
# needs --workers >= 4 and --tde on/on_wal.

import logging
import time

import stormweaver as sw
from stormweaver import scenario
from stormweaver.entropy import db_files_entropy

logger = logging.getLogger("scenario.basic")


def main(args):
    # scenario.parse handles the common options every scenario shares:
    # --duration, --workers, --repeat, --tde, --pgsm, --clear-logs.
    # extend= customizes the parser; this demo encrypts by default.
    opts = scenario.parse(args, extend=lambda p: p.set_defaults(tde="on"))

    # worker 1/2/3 and workers[2] below are hardcoded, so fewer workers breaks the demo
    if opts.workers < 4:
        raise SystemExit("this demo customizes workers 1-3, run with --workers >= 4")
    # db_files_entropy needs pg_tde_is_encrypted, only present with tde on
    if opts.tde == "off":
        raise SystemExit("this demo requires pg_tde; use --tde on or on_wal")

    # Actions (mostly SQL statement generators) live in registries.
    # default_action_registry() returns THE process-wide default -
    # modifying it affects everything COPIED from it afterwards, and
    # nothing copied before.
    default_registry = sw.default_action_registry()

    # Each worker copies the workload's registry at construction;
    # worker_setup then customizes individual workers. Indices are
    # 0-based. Runs again for every cycle (workers are per-cycle).
    def worker_setup(worker, idx):
        actions = worker.possible_actions()
        if idx == 1:
            # worker 1 swaps to the CURRENT default registry: it gets
            # truncate_table_demo (added below, before any cycle starts),
            # but loses the framework's standard actions
            actions.use(default_registry)
        elif idx == 2:
            # crank up alter_table on worker 2
            actions.get("alter_table").weight = 127
        elif idx == 3:
            # worker 3 gets an extra action; not table based
            actions.make_custom_sql("demo_checkpoint", "CHECKPOINT;", 1)

    # single_pg does what every scenario needs: fresh datadir (and keyring -
    # pg_tde keys must match the cluster they were created for), server
    # config incl. shared_preload_libraries for tde/pgsm, createdb, tde
    # init, a registry with standard actions (checkpoint, vacuum full,
    # truncate, reindex; partition actions trimmed), initial tables, and a
    # ready-to-run workload built from --duration/--workers.
    with scenario.single_pg(opts, worker_setup=worker_setup) as ctx:
        # Registries COPY on use(): ctx.registry snapshotted the default
        # when the context was entered, so an action added to the default
        # NOW is invisible to ctx.registry - only worker 1 (which re-copies
        # the default at each cycle start) will run it.
        default_registry.make_custom_table_sql(
            "truncate_table_demo", "TRUNCATE {table};", 2
        )

        for cycle in range(opts.repeat):
            # start() launches the worker threads and returns; the demo uses
            # the running window to modify a worker mid-flight
            ctx.workload.start()
            time.sleep(1)
            if cycle == 0:
                # one second in, stop worker 2 from running alter_table at
                # all - registries can be modified while the worker runs
                ctx.workload.workers[2].possible_actions().remove("alter_table")
            ctx.workload.wait()

            # Restart the server; next cycle's workers connect fresh, and
            # mid-cycle restarts would be survived too (workers reconnect).
            ctx.restart_and_wait()

            # Verify encryption on disk: every tde_heap relation file must
            # be encrypted, everything else must not be.
            db_files_entropy(ctx.connect("verification"), ctx.datadir)
            logger.info("cycle %d/%d verified", cycle + 1, opts.repeat)

    print("Demo scenario completed successfully")

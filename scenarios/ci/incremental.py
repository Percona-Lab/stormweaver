# Incremental backup test: after each workload cycle take an incremental
# basebackup (PG 17+), then restore every backup chain prefix with
# pg_combinebackup and verify the database checksums match what was
# captured when the backup was taken.

import logging
import shutil
from pathlib import Path

from stormweaver import scenario

logger = logging.getLogger("scenario.incremental")


def add_arguments(parser):
    scenario.add_common_arguments(parser)


def main(args):
    opts = scenario.finalize(args)
    scenario.fresh_dir("backups", "archive")

    # archive=True also enables summarize_wal, required for incremental
    # basebackups
    with scenario.single_pg(opts, archive=True) as ctx:
        logger.info("taking base backup")
        ctx.pg.basebackup("backups/backup_0", extra_args=["-c", "fast"])

        for i in range(1, opts.repeat + 1):
            logger.info("cycle %d/%d: workload + incremental backup", i, opts.repeat)
            ctx.workload.run()

            prev = f"backups/backup_{i - 1}"
            backup = f"backups/backup_{i}"
            ctx.pg.basebackup(
                backup,
                extra_args=["-c", "fast"],
                incremental=f"{prev}/backup_manifest",
            )

            w = ctx.make_worker("verification")
            w.calculate_database_checksums(f"{backup}.checksum")
            logger.info("cycle %d/%d backed up", i, opts.repeat)

        chain = ["backups/backup_0"]
        for i in range(1, opts.repeat + 1):
            logger.info("Restoring and verifying incremental #%d", i)
            ctx.pg.stop()
            shutil.rmtree(ctx.datadir)

            chain.append(f"backups/backup_{i}")
            ctx.pg.combinebackup(chain, ctx.datadir)

            ctx.pg.start()
            if not ctx.pg.wait_ready():
                raise RuntimeError(f"restored server #{i} did not become ready")

            w = ctx.make_worker("reset")
            w.reset_metadata()
            w.discover_existing_schema()
            restored = Path(ctx.datadir) / "db.checksum"
            w.calculate_database_checksums(str(restored))

            scenario.compare_checksums(
                restored, f"backups/backup_{i}.checksum", f"incremental #{i}"
            )

    print("Incremental backup scenario completed successfully")

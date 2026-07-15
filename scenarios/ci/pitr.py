# PITR test: run workload cycles with WAL archiving, checksum the database
# at known timestamps, then for each timestamp restore the base backup and
# recover to that point in time, verifying the checksums match.

import logging
import shutil
import time
from pathlib import Path

from stormweaver import scenario

logger = logging.getLogger("scenario.pitr")

RECOVERY_PAUSE_MSG = "pausing at the end of recovery"
RECOVERY_TIMEOUT = 200


def add_arguments(parser):
    scenario.add_common_arguments(parser)


def main(args):
    opts = scenario.finalize(args)
    scenario.fresh_dir("backups", "archive")

    with scenario.single_pg(opts, archive=True) as ctx:
        ctx.pg.basebackup("backups/backup_0", extra_args=["-c", "fast"])

        times = []
        # one extra cycle: it generates WAL past every restore target, so
        # recovery can always reach recovery_target_time
        for i in range(opts.repeat + 1):
            ctx.workload.run()

            # settle, then stamp: the checksum must describe the database
            # exactly as it is at the recorded second
            time.sleep(3)
            t = int(time.time())
            w = ctx.make_worker("verification")
            w.calculate_database_checksums(f"backups/{t}.checksum")
            times.append(t)
            # recovery_target_time only has one-second granularity, next
            # cycle's writes must not land in the same wall-clock second
            time.sleep(3)
            logger.info("cycle %d/%d checksummed at %d", i + 1, opts.repeat + 1, t)

        # recovery consumes/pollutes the archive (new timeline history and
        # WAL get archived after promotion), keep a pristine copy
        shutil.rmtree("archive-copy", ignore_errors=True)
        shutil.copytree("archive", "archive-copy")

        for i, t in enumerate(times[: opts.repeat], start=1):
            logger.info("Restoring and verifying PITR #%d - %d", i, t)
            ctx.pg.stop()

            shutil.rmtree("archive")
            shutil.copytree("archive-copy", "archive")

            shutil.rmtree(ctx.datadir)
            shutil.copytree("backups/backup_0", ctx.datadir)
            # own archive: single_pg pointed archive_dir at ./archive
            ctx.pg.enable_restoring(ctx.pg, signal="recovery")

            # assumes postgres runs in the host timezone, same as the lua original
            target = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t))
            ctx.pg.add_config({"recovery_target_time": target})

            # the server log file persists across restarts, capture the offset
            # now or wait_for_log below could match a stale pause message from
            # a previous restore iteration
            log_path = ctx.pg.server_log_path
            offset = log_path.stat().st_size if log_path.exists() else 0
            ctx.pg.start()

            if not scenario.wait_for_log(
                log_path, RECOVERY_PAUSE_MSG, RECOVERY_TIMEOUT, offset=offset
            ):
                raise RuntimeError(
                    f"PITR #{i} (target {target}) did not reach the recovery "
                    f"pause; see {log_path}"
                )

            conn = ctx.connect("resume")
            res = conn.execute("SELECT pg_wal_replay_resume()")
            if not res.success():
                raise RuntimeError(
                    f"pg_wal_replay_resume failed for PITR #{i} (target {target}): "
                    f"{res.error_message}"
                )

            w = ctx.make_worker("reset")
            w.reset_metadata()
            w.discover_existing_schema()
            restored = Path(ctx.datadir) / "db.checksum"
            w.calculate_database_checksums(str(restored))

            scenario.compare_checksums(restored, f"backups/{t}.checksum", f"PITR #{i}")

    print("PITR scenario completed successfully")

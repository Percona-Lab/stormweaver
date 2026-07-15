from pathlib import Path
from types import TracebackType
from typing import Self

from stormweaver.testing.node import PgTestNode
from stormweaver.testing.process import command_ok


class RewindDriver:
    """Driver mirroring RewindTest.pm: setup -> standby -> promote -> rewind.

    Generic over the database flavor. Subclasses override the setup hooks and
    rewind_bin/rewind_user to specialize.
    """

    rewind_bin = "pg_rewind"

    def __init__(self, install_dir: str | Path, *, debug: bool = False) -> None:
        self.install_dir = install_dir
        self.debug = debug
        self.primary: PgTestNode | None = None
        self.standby: PgTestNode | None = None

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        for node in (self.standby, self.primary):
            if node is not None:
                node.close()

    # specialization hooks

    def primary_config(self) -> dict[str, str]:
        # wal_log_hints: pg_rewind needs it (or checksums) to find the divergence
        # wal_keep_size: keep old WAL so trace-back doesn't hit a recycled segment
        return {"wal_log_hints": "on", "wal_keep_size": "320MB"}

    def initdb_args(self) -> list[str] | None:
        return None

    def configure_primary(self, primary: PgTestNode) -> None:
        # post-fresh setup (extensions, keys, roles); default none
        pass

    def rewind_user(self) -> str | None:
        # remote source connects as this role; None -> default superuser
        return None

    # lifecycle

    def setup_primary(self) -> PgTestNode:
        primary = PgTestNode.fresh(
            self.install_dir,
            name="rwprimary",
            config=self.primary_config(),
            initdb_args=self.initdb_args(),
        )
        self.configure_primary(primary)
        self.primary = primary
        return primary

    def create_standby(self) -> PgTestNode:
        assert self.primary is not None
        backup = self.primary.basebackup()
        self.standby = PgTestNode.from_backup(backup, self.primary, name="rwstandby")
        return self.standby

    def promote_standby(self) -> None:
        assert self.primary is not None and self.standby is not None
        self.primary.wait_for_catchup(self.standby)
        self.standby.promote()
        self.standby.poll_until("SELECT NOT pg_is_in_recovery()", "t", timeout=15)

    def run_rewind(self, mode: str) -> None:
        assert self.primary is not None and self.standby is not None
        primary, standby = self.primary, self.standby
        user = self.rewind_user()
        connstr = f"host=localhost port={standby.port} dbname=postgres"
        if user:
            connstr += f" user={user}"
        debug = ["--debug"] if self.debug else []
        # non-archive modes stop immediately so the target needs crash recovery
        primary.stop(mode="fast" if mode == "archive" else "immediate")

        with primary.db.preserve_config() as saved_conf:
            if mode == "local":
                standby.stop()
                cp = primary.run_bin(
                    self.rewind_bin,
                    *debug,
                    f"--source-pgdata={standby.datadir}",
                    f"--target-pgdata={primary.datadir}",
                    "--no-sync",
                    f"--config-file={saved_conf}",
                )
                command_ok(cp, f"{self.rewind_bin} local")
            elif mode == "remote":
                cp = primary.run_bin(
                    self.rewind_bin,
                    *debug,
                    "--source-server",
                    connstr,
                    f"--target-pgdata={primary.datadir}",
                    "--no-sync",
                    "--write-recovery-conf",
                    f"--config-file={saved_conf}",
                )
                command_ok(cp, f"{self.rewind_bin} remote")
                assert (primary.datadir / "standby.signal").exists()
                if user:
                    standby.safe_sql(f"ALTER ROLE {user} WITH REPLICATION")
            elif mode == "archive":
                primary.db.move_wal_to_archive()
                primary.db.enable_restoring(primary.db)
                standby.stop()
                cp = primary.run_bin(
                    self.rewind_bin,
                    *debug,
                    f"--source-pgdata={standby.datadir}",
                    f"--target-pgdata={primary.datadir}",
                    "--no-sync",
                    "--no-ensure-shutdown",
                    "--restore-target-wal",
                    f"--config-file={primary.datadir}/postgresql.conf",
                )
                command_ok(cp, f"{self.rewind_bin} archive")
            else:
                raise ValueError(f"unknown rewind mode: {mode}")

        # --write-recovery-conf already wrote standby.signal + auto.conf (which
        # preserve_config does not touch); local/archive need manual rejoin
        if mode == "remote":
            primary.start()
        else:
            # local/archive stop the source for the copy; bring the new primary
            # back up so the rewound node can stream from it
            standby.start()
            primary.rejoin_as_standby(standby)
        standby.wait_for_catchup(primary)

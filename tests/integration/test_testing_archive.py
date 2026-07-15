import shutil
import time

import stormweaver as sw
import stormweaver.testing as st
from conftest import PG_DIR, requires_pg

pytestmark = requires_pg


def wait_for_file(path, timeout=30.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return True
        time.sleep(0.5)
    return False


def test_enable_archiving_archives_segments():
    with st.PgTestNode.fresh(PG_DIR, name="archsrc") as node:
        # archive_mode needs a server restart to take effect
        node.db.enable_archiving()
        node.restart()
        node.safe_sql("CREATE TABLE t (d text)")
        node.safe_sql("INSERT INTO t VALUES ('x')")
        seg = node.sql_value("SELECT pg_walfile_name(pg_switch_wal())")
        assert wait_for_file(node.db.archive_dir / seg), f"{seg} not archived"


def test_archive_recovery_from_basebackup():
    with st.PgTestNode.fresh(PG_DIR, name="arcprim") as primary:
        primary.db.enable_archiving()
        primary.restart()
        primary.safe_sql("CREATE TABLE t (d text)")
        primary.safe_sql("INSERT INTO t VALUES ('in backup')")
        backup = primary.basebackup()
        try:
            primary.safe_sql("INSERT INTO t VALUES ('after backup')")
            seg = primary.sql_value("SELECT pg_walfile_name(pg_switch_wal())")
            assert wait_for_file(primary.db.archive_dir / seg), f"{seg} not archived"

            port = st.alloc_port()
            pg = sw.Postgres(install_dir=PG_DIR, datadir=backup, init=False, port=port)
            # backup carries the primary's config; override port and keep the
            # socket outside the datadir (appended settings win, last wins)
            pg.add_config(
                {
                    "port": str(port),
                    "unix_socket_directories": str(backup.resolve().parent),
                }
            )
            pg.enable_restoring(primary.db, signal="recovery")
            with st.PgTestNode(pg, "postgres", "arcrest") as restored:
                restored.start()
                # replays archived WAL past the backup, then promotes
                restored.poll_until("SELECT NOT pg_is_in_recovery()", "t", timeout=60)
                rows = [r[0] for r in restored.safe_sql("SELECT d FROM t ORDER BY d")]
                assert rows == ["after backup", "in backup"]
        finally:
            # basebackup() puts the backup in its own /tmp/sw-backup-* dir
            shutil.rmtree(backup.parent, ignore_errors=True)

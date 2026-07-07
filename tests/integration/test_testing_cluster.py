import stormweaver.testing as st
from conftest import PG_DIR, requires_pg

pytestmark = requires_pg


def test_standby_catchup_and_promote():
    with st.PgTestNode.fresh(PG_DIR, name="cprimary") as primary:
        primary.safe_sql("CREATE TABLE t (d text)")
        primary.safe_sql("INSERT INTO t VALUES ('before backup')")
        backup = primary.basebackup()
        with st.PgTestNode.from_backup(backup, primary, name="cstandby") as standby:
            primary.safe_sql("INSERT INTO t VALUES ('after backup')")
            primary.wait_for_catchup(standby)
            rows = [r[0] for r in standby.safe_sql("SELECT d FROM t ORDER BY d")]
            assert rows == ["after backup", "before backup"]

            standby.expect_error("read-only", "INSERT INTO t VALUES ('nope')")
            standby.promote()
            standby.poll_until("SELECT NOT pg_is_in_recovery()", "t", timeout=15)
            standby.safe_sql("INSERT INTO t VALUES ('on promoted')")

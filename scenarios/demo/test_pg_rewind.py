import stormweaver.testing as st
from conftest import PG_DIR, requires_pg

pytestmark = requires_pg


def test_pg_rewind_basic():
    assert PG_DIR is not None
    # wal_log_hints: required for pg_rewind on clusters without checksums.
    # wal_keep_size: keep old WAL segments so pg_rewind can trace back to the
    # divergence point on the old primary instead of hitting a recycled segment.
    with st.PgTestNode.fresh(
        PG_DIR,
        name="rwprimary",
        config={"wal_log_hints": "on", "wal_keep_size": "128MB"},
    ) as primary:
        primary.safe_sql("CREATE TABLE tbl1 (d text)")
        primary.safe_sql("INSERT INTO tbl1 VALUES ('in primary')")
        primary.safe_sql("CHECKPOINT")

        backup = primary.basebackup()
        with st.PgTestNode.from_backup(backup, primary, name="rwstandby") as standby:
            primary.safe_sql("INSERT INTO tbl1 VALUES ('in primary, before promotion')")
            primary.wait_for_catchup(standby)

            standby.promote()
            standby.poll_until("SELECT NOT pg_is_in_recovery()", "t", timeout=15)

            # diverge both timelines
            primary.safe_sql(
                "INSERT INTO tbl1 VALUES ('in old primary, after promotion')"
            )
            standby.safe_sql("INSERT INTO tbl1 VALUES ('in standby, after promotion')")

            primary.stop()
            cp = st.run_bin(
                PG_DIR,
                "pg_rewind",
                "--target-pgdata",
                primary.datadir,
                "--source-server",
                f"host=localhost port={standby.port} dbname=postgres",
            )
            st.command_ok(cp, "pg_rewind")

            # rewound old primary rejoins as a standby following the new primary
            primary.rejoin_as_standby(standby)

            standby.wait_for_catchup(primary)
            rows = [r[0] for r in primary.safe_sql("SELECT d FROM tbl1 ORDER BY d")]
            assert "in standby, after promotion" in rows
            assert "in old primary, after promotion" not in rows
            assert "in primary, before promotion" in rows


if __name__ == "__main__":
    st.main()

import stormweaver.testing as st
from conftest import PG_DIR, requires_pg
from rewind import RewindDriver

pytestmark = requires_pg


def test_pg_rewind_basic():
    with RewindDriver(PG_DIR, debug=True) as rw:
        p = rw.setup_primary()
        p.safe_sql("CREATE TABLE tbl1 (d text)")
        p.safe_sql("INSERT INTO tbl1 VALUES ('in primary')")
        p.safe_sql("CHECKPOINT")

        s = rw.create_standby()
        p.safe_sql("INSERT INTO tbl1 VALUES ('in primary, before promotion')")

        rw.promote_standby()

        # diverge both timelines
        p.safe_sql("INSERT INTO tbl1 VALUES ('in old primary, after promotion')")
        s.safe_sql("INSERT INTO tbl1 VALUES ('in standby, after promotion')")

        rw.run_rewind("remote")

        rows = [r[0] for r in p.safe_sql("SELECT d FROM tbl1 ORDER BY d")]
        assert "in standby, after promotion" in rows
        assert "in old primary, after promotion" not in rows
        assert "in primary, before promotion" in rows


if __name__ == "__main__":
    st.main()

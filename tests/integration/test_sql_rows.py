import stormweaver as sw
from conftest import pg_user, requires_pg, running_pg

pytestmark = requires_pg


def test_rows_returns_result_data():
    with running_pg(26320, "rowsdb") as pg:
        conn = sw.connect_pg(
            host="localhost",
            port=pg.port,
            dbname="rowsdb",
            user=pg_user(),
            log_name="rows-test",
        )
        res = conn.execute(
            "SELECT 1, 'a', NULL UNION ALL SELECT 2, 'b', 'x' ORDER BY 1"
        )
        assert res.success()
        expected = [["1", "a", None], ["2", "b", "x"]]
        assert res.rows() == expected
        # second call must return the same data, not fabricated rows
        assert res.rows() == expected

        ddl = conn.execute("CREATE TABLE t(i int)")
        assert ddl.success()
        assert ddl.rows() == []

        bad = conn.execute("SELECT * FROM nope")
        assert not bad.success()
        assert bad.rows() == []

import pytest
import stormweaver as sw
import stormweaver.testing as st
from conftest import PG_DIR, requires_pg

pytestmark = requires_pg


def test_node_sql_surface():
    with st.PgTestNode.fresh(PG_DIR, name="surface") as node:
        node.safe_sql("CREATE TABLE t (a int, b text)")
        node.safe_sql("INSERT INTO t VALUES ($1, $2)", (1, "one"))
        assert node.safe_sql("SELECT b FROM t WHERE a = $1", (1,)) == [["one"]]
        assert node.sql_value("SELECT count(*) FROM t") == "1"

        with pytest.raises(sw.SqlError):
            node.safe_sql("SELECT * FROM nope")

        r = node.sql("SELECT * FROM nope")
        assert not r.success()

        node.expect_error("does not exist", "SELECT * FROM nope")
        with pytest.raises(AssertionError):
            node.expect_error("no match here", "SELECT * FROM nope")
        with pytest.raises(AssertionError):
            node.expect_error("anything", "SELECT 1")
        with pytest.raises(AssertionError):
            node.sql_value("SELECT * FROM t")  # 1 row x 2 cols -> not 1x1


def test_node_lifecycle_reconnect():
    with st.PgTestNode.fresh(PG_DIR, name="lifecycle") as node:
        node.safe_sql("CREATE TABLE t (a int)")
        node.restart()
        node.safe_sql("INSERT INTO t VALUES (1)")  # default conn re-established
        node.kill()
        node.start()
        # committed autocommit insert must survive the crash
        assert node.sql_value("SELECT count(*) FROM t") == "1"


def test_extra_sessions_and_raw():
    with st.PgTestNode.fresh(PG_DIR, name="sessions", dbname="postgres") as node:
        node.createdb("other")
        with node.connect(db="other") as conn:
            conn.safe_sql("CREATE TABLE only_here (a int)")
        with pytest.raises(RuntimeError, match="connection closed"):
            conn.sql("SELECT 1")
        node.expect_error("does not exist", "SELECT * FROM only_here")

        raw = node.raw_connect("rawtest")
        assert raw.safe_execute("SELECT 1").rows() == [["1"]]

        # exiting the with-block closed the session, so the db is droppable
        node.safe_sql("DROP DATABASE other")


def test_cleanup_removes_dir():
    node = st.PgTestNode.fresh(PG_DIR, name="cleanup")
    d = node.datadir
    assert d.exists()
    node.close()
    assert not d.exists()


def test_poll_start_after_kill():
    with st.PgTestNode.fresh(PG_DIR, name="pollstart") as node:
        node.safe_sql("CREATE TABLE t (x int)")
        node.safe_sql("INSERT INTO t VALUES (1)")
        node.kill()
        node.poll_start()
        assert node.sql_value("SELECT count(*) FROM t") == "1"

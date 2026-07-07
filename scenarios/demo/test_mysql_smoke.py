import stormweaver.testing as st
from conftest import MYSQL_DIR, requires_mysql

pytestmark = requires_mysql


def test_mysql_params_smoke():
    assert MYSQL_DIR is not None
    with st.MySqlTestNode.fresh(MYSQL_DIR, name="smoke") as node:
        node.safe_sql("CREATE TABLE t (a INT, b TEXT)")
        node.safe_sql("INSERT INTO t VALUES (?, ?)", (1, "it's"))
        assert node.safe_sql("SELECT b FROM t WHERE a = ?", (1,)) == [["it's"]]
        assert node.sql_value("SELECT count(*) FROM t") == "1"
        node.expect_error("exist", "DROP TABLE nope")


if __name__ == "__main__":
    st.main()

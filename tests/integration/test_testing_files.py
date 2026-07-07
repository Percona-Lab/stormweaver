import pytest
import stormweaver.testing as st
from conftest import PG_DIR, requires_pg

pytestmark = requires_pg


def test_relation_file_contents():
    with st.PgTestNode.fresh(PG_DIR, name="files") as node:
        node.safe_sql("CREATE TABLE t (d text)")
        node.safe_sql("INSERT INTO t VALUES ('stormweaver_marker_xyz')")
        node.safe_sql("CHECKPOINT")
        data = node.read_relation_file("t")
        assert b"stormweaver_marker_xyz" in data


def test_wait_for_log_and_offset():
    with st.PgTestNode.fresh(PG_DIR, name="logwait") as node:
        node.wait_for_log(r"database system is ready", timeout=10)
        offset = node.log_offset()
        with pytest.raises(TimeoutError):
            node.wait_for_log(r"no such message ever", offset=offset, timeout=1)
        node.restart()
        node.wait_for_log(r"database system is ready", offset=offset, timeout=10)


def test_poll_until():
    with st.PgTestNode.fresh(PG_DIR, name="poll") as node:
        node.poll_until("SELECT true", "t", timeout=5)
        with pytest.raises(TimeoutError):
            node.poll_until("SELECT false", "t", timeout=1, interval=0.2)


def test_psql_sql_and_file_guard(tmp_path):
    with st.PgTestNode.fresh(PG_DIR, name="guard") as node:
        f = tmp_path / "x.sql"
        f.write_text("SELECT 1;")
        with pytest.raises(ValueError):
            node.psql("SELECT 2;", file=f)

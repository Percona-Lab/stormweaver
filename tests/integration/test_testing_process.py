import pytest
import stormweaver.testing as st
from conftest import PG_DIR, requires_pg

pytestmark = requires_pg


def test_run_bin_and_asserts():
    with st.PgTestNode.fresh(PG_DIR, name="proc") as node:
        cp = st.run_bin(PG_DIR, "pg_controldata", str(node.datadir))
        st.command_ok(cp)
        assert "Database cluster state" in cp.stdout

        with pytest.raises(RuntimeError):
            st.run_bin(PG_DIR, "not_a_binary")

        bad = node.run_bin("psql", "-c", "SELECT * FROM nope")
        st.command_fails(bad, stderr_re="does not exist")
        with pytest.raises(AssertionError):
            st.command_ok(bad)
        with pytest.raises(AssertionError):
            st.command_fails(cp)  # succeeded -> must raise
        with pytest.raises(AssertionError):
            st.command_fails(bad, stderr_re="different error text")


def test_node_psql():
    with st.PgTestNode.fresh(PG_DIR, name="psqlnode") as node:
        cp = node.psql("SELECT 1 AS one;")
        st.command_ok(cp)
        assert "1" in cp.stdout

        bad = node.psql("SELECT 1; SELECT * FROM nope;")
        st.command_fails(bad, stderr_re="does not exist")  # ON_ERROR_STOP default

        ok = node.psql("SELECT * FROM nope;", on_error_stop=False)
        assert ok.returncode == 0  # psql without ON_ERROR_STOP exits 0 on SQL errors

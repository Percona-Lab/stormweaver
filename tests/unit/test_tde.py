import pytest
from stormweaver import tde


class FakeResult:
    def __init__(self, ok=True):
        self._ok = ok
        self.error_message = "" if ok else "boom"

    def success(self):
        return self._ok


class FakeConn:
    def __init__(self):
        self.queries = []

    def execute(self, query):
        self.queries.append(query)
        return FakeResult()


class FailingConn(FakeConn):
    def execute(self, query):
        super().execute(query)
        return FakeResult(ok=False)


def test_init_tde_only_for_db_statements():
    conn = FakeConn()
    tde.init_tde_only_for_db(conn, "/tmp/k.per")
    assert len(conn.queries) == 5
    assert conn.queries[0].startswith("CREATE EXTENSION")
    assert "default_table_access_method = tde_heap" in conn.queries[1]
    assert (
        "pg_tde_add_database_key_provider_file('reg_file', '/tmp/k.per')"
        in conn.queries[2]
    )
    assert "pg_tde_create_key_using_database_key_provider" in conn.queries[3]
    assert "pg_tde_set_key_using_database_key_provider" in conn.queries[4]


def test_init_tde_globally_statements():
    conn = FakeConn()
    tde.init_tde_globally(conn, "/tmp/g.per")
    assert len(conn.queries) == 8
    assert conn.queries[0].startswith("CREATE EXTENSION")
    assert "default_table_access_method = tde_heap" in conn.queries[1]
    assert (
        "pg_tde_add_global_key_provider_file('reg_file', '/tmp/g.per')"
        in conn.queries[2]
    )
    assert "pg_tde_create_key_using_global_key_provider" in conn.queries[3]
    assert "'server-principal-key'" in conn.queries[3]
    assert "pg_tde_set_key_using_global_key_provider" in conn.queries[4]
    assert "pg_tde_set_server_key_using_global_key_provider" in conn.queries[5]
    assert "pg_tde_set_default_key_using_global_key_provider" in conn.queries[6]
    assert "'def-principal-key'" in conn.queries[6]
    assert "pg_tde.wal_encrypt" in conn.queries[7]


def test_failure_raises():
    with pytest.raises(RuntimeError, match="boom"):
        tde.init_tde_only_for_db(FailingConn(), "/k")

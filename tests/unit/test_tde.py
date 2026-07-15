from pathlib import Path

import pytest
from stormweaver import tde
from stormweaver.keyrings.file import FileKeyring
from stormweaver.tde import PgTde


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
    assert len(conn.queries) == 9
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
    # default key must exist before it can be set as default
    assert "pg_tde_create_key_using_global_key_provider" in conn.queries[6]
    assert "'def-principal-key'" in conn.queries[6]
    assert "pg_tde_set_default_key_using_global_key_provider" in conn.queries[7]
    assert "'def-principal-key'" in conn.queries[7]
    assert "pg_tde.wal_encrypt" in conn.queries[8]


def test_failure_raises():
    with pytest.raises(RuntimeError, match="boom"):
        tde.init_tde_only_for_db(FailingConn(), "/k")


def test_init_tde_with_keyring_object():
    from pathlib import Path

    from stormweaver.keyrings.vault import VaultKeyring

    conn = FakeConn()
    kr = VaultKeyring(
        url="https://127.0.0.1:8200",
        mount_path="secret",
        token_file=Path("/w/token"),
        ca_cert=Path("/w/ca.pem"),
    )
    tde.init_tde_only_for_db(conn, kr)
    assert "pg_tde_add_database_key_provider_vault_v2" in conn.queries[2]
    assert "'https://127.0.0.1:8200'" in conn.queries[2]

    conn2 = FakeConn()
    tde.init_tde_globally(conn2, kr)
    assert "pg_tde_add_global_key_provider_vault_v2" in conn2.queries[2]


class FakeNode:
    def __init__(self, values=None):
        self.queries = []
        self._values = values or {}
        self.disk = {}  # table -> bytes, returned by read_relation_file

    def safe_sql(self, query, params=None):
        self.queries.append(query)
        return []

    def sql_value(self, query, params=None):
        self.queries.append(query)
        return self._values.get(query)

    def read_relation_file(self, table):
        return self.disk.get(table, b"")


def test_pgtde_setup_wal_matches_rewind_sequence():
    node = FakeNode()
    kr = FileKeyring(Path("/tmp/k.per"))
    PgTde(node, kr).setup(wal=True)
    q = node.queries
    assert q[0].startswith("CREATE EXTENSION")
    assert "pg_tde_add_global_key_provider_file('wal-provider'" in q[1]
    assert (
        "pg_tde_create_key_using_global_key_provider('wal-key', 'wal-provider')" in q[2]
    )
    assert (
        "pg_tde_set_server_key_using_global_key_provider('wal-key', 'wal-provider')"
        in q[3]
    )
    assert "pg_tde_add_database_key_provider_file('db-provider'" in q[4]
    assert (
        "pg_tde_create_key_using_database_key_provider('db-key', 'db-provider')" in q[5]
    )
    assert "pg_tde_set_key_using_database_key_provider('db-key', 'db-provider')" in q[6]
    assert "pg_tde.wal_encrypt = on" in q[7]


def test_pgtde_setup_database_only():
    node = FakeNode()
    kr = FileKeyring(Path("/tmp/k.per"))
    PgTde(node, kr).setup(scope="database")
    joined = "\n".join(node.queries)
    assert "wal_encrypt" not in joined
    assert "global_key_provider" not in joined
    assert "pg_tde_add_database_key_provider_file('provider'" in node.queries[1]


def test_pgtde_add_provider_keyring_override():
    node = FakeNode()
    default_kr = FileKeyring(Path("/tmp/default.per"))
    other_kr = FileKeyring(Path("/tmp/other.per"))
    tde = PgTde(node, default_kr)
    tde.add_provider("global", "g", keyring=other_kr)
    assert "/tmp/other.per" in node.queries[0]


def test_pgtde_is_encrypted_bool():
    node = FakeNode(values={"SELECT pg_tde_is_encrypted('t1')": "t"})
    assert PgTde(node, FileKeyring(Path("/k"))).is_encrypted("t1") is True
    node2 = FakeNode(values={"SELECT pg_tde_is_encrypted('t1')": "f"})
    assert PgTde(node2, FileKeyring(Path("/k"))).is_encrypted("t1") is False


def test_pgtde_assert_on_disk():
    node = FakeNode()
    node.disk["plain"] = b"...multitude..."
    tde = PgTde(node, FileKeyring(Path("/k")))
    tde.assert_on_disk("plain", "multitud")
    node.disk["enc"] = b"\x00\x01\x02ciphertext"
    tde.assert_not_on_disk("enc", "multitud")

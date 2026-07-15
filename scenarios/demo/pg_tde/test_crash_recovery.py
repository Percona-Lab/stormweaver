from pathlib import Path

import stormweaver.testing as st
from _pgtde import TDE_DIR, requires_pg_tde
from stormweaver.keyrings.file import FileKeyring
from stormweaver.tde import PgTde

pytestmark = requires_pg_tde


def test_crash_recovery(tmp_path: Path):
    kr_global = FileKeyring(tmp_path / "global.keys")
    kr_db = FileKeyring(tmp_path / "db.keys")
    with st.PgTestNode.fresh(
        TDE_DIR,
        name="crash",
        config={
            "shared_preload_libraries": "pg_tde",
            "checkpoint_timeout": "1h",
        },
    ) as node:
        tde = PgTde(node, kr_db)
        tde.create_extension()

        tde.add_provider("global", "global_keyring", keyring=kr_global)
        tde.create_key("wal_encryption_key", "global_keyring", scope="global")
        tde.set_server_key("wal_encryption_key", "global_keyring")

        tde.add_provider("database", "db_keyring")
        tde.create_key("db_key", "db_keyring", scope="database")
        tde.set_key("db_key", "db_keyring")

        node.safe_sql("CREATE TABLE test_enc (x int PRIMARY KEY) USING tde_heap")
        node.safe_sql("INSERT INTO test_enc (x) VALUES (1), (2)")
        node.safe_sql("CREATE TABLE test_plain (x int PRIMARY KEY) USING heap")
        node.safe_sql("INSERT INTO test_plain (x) VALUES (3), (4)")
        tde.enable_wal_encryption()

        node.kill()
        node.poll_start()  # crash recovery of the pg_tde setup

        # rotate wal + db key, insert
        tde.create_key("wal_encryption_key_1", "global_keyring", scope="global")
        tde.set_server_key("wal_encryption_key_1", "global_keyring")
        tde.create_key("db_key_1", "db_keyring", scope="database")
        tde.set_key("db_key_1", "db_keyring")
        node.safe_sql("INSERT INTO test_enc (x) VALUES (3), (4)")

        node.kill()
        node.poll_start()

        tde.create_key("wal_encryption_key_2", "global_keyring", scope="global")
        tde.set_server_key("wal_encryption_key_2", "global_keyring")
        tde.create_key("db_key_2", "db_keyring", scope="database")
        tde.set_key("db_key_2", "db_keyring")
        node.safe_sql("INSERT INTO test_enc (x) VALUES (5), (6)")

        node.kill()
        node.poll_start()

        assert [r[0] for r in node.safe_sql("TABLE test_enc")] == [
            "1",
            "2",
            "3",
            "4",
            "5",
            "6",
        ]

        node.safe_sql("CREATE TABLE test_enc2 (x int PRIMARY KEY) USING tde_heap")

        node.kill()
        node.poll_start()  # redo of smgr internal key creation

        node.safe_sql("INSERT INTO test_enc (x) VALUES (7), (8)")

        node.kill()
        # redo when cipher changed after the crash
        node.db.add_config({"pg_tde.cipher": "aes_256"})
        node.poll_start()

        assert [r[0] for r in node.safe_sql("TABLE test_enc")] == [
            "1",
            "2",
            "3",
            "4",
            "5",
            "6",
            "7",
            "8",
        ]

        # encrypted unlogged sequence: init fork key must survive WAL replay
        node.safe_sql("CREATE UNLOGGED SEQUENCE seq_unlogged OWNED BY test_enc.x")
        node.safe_sql("SELECT nextval('seq_unlogged')")
        node.safe_sql("SELECT nextval('seq_unlogged')")
        assert tde.is_encrypted("seq_unlogged")

        node.kill()
        node.poll_start()

        assert node.sql_value("SELECT nextval('seq_unlogged')") == "1"


if __name__ == "__main__":
    st.main()

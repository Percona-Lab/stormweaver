from pathlib import Path

import stormweaver.testing as st
from _pgtde import TDE_DIR, requires_pg_tde
from stormweaver.keyrings.file import FileKeyring
from stormweaver.tde import PgTde

pytestmark = requires_pg_tde


def test_tde_heap(tmp_path: Path):
    keyring = FileKeyring(tmp_path / "db.keys")
    with st.PgTestNode.fresh(
        TDE_DIR, name="tdeheap", config={"shared_preload_libraries": "pg_tde"}
    ) as node:
        tde = PgTde(node, keyring)
        tde.setup(scope="database")

        # test_enc1: create with tde_heap
        node.safe_sql(
            "CREATE TABLE test_enc1 (id SERIAL, k VARCHAR(32), PRIMARY KEY (id))"
            " USING tde_heap"
        )
        node.safe_sql(
            "INSERT INTO test_enc1 (k) VALUES ('multitude'), ('multitudinous')"
        )

        # test_enc2: create heap, alter to tde_heap
        node.safe_sql(
            "CREATE TABLE test_enc2 (id SERIAL, k VARCHAR(32), PRIMARY KEY (id))"
        )
        node.safe_sql(
            "INSERT INTO test_enc2 (k) VALUES ('multitude'), ('multitudinous')"
        )
        node.safe_sql("ALTER TABLE test_enc2 SET ACCESS METHOD tde_heap")

        # test_enc3: session default access method
        node.safe_sql("SET default_table_access_method = 'tde_heap'")
        node.safe_sql(
            "CREATE TABLE test_enc3 (id SERIAL, k VARCHAR(32), PRIMARY KEY (id))"
        )
        node.safe_sql(
            "INSERT INTO test_enc3 (k) VALUES ('multitude'), ('multitudinous')"
        )
        node.safe_sql("RESET default_table_access_method")

        has_am_default = node.server_version_num >= 170000
        if has_am_default:
            # test_enc4: heap, then SET ACCESS METHOD DEFAULT with default=tde_heap
            node.safe_sql(
                "CREATE TABLE test_enc4 (id SERIAL, k VARCHAR(32), PRIMARY KEY (id))"
                " USING heap"
            )
            node.safe_sql(
                "INSERT INTO test_enc4 (k) VALUES ('multitude'), ('multitudinous')"
            )
            node.safe_sql("SET default_table_access_method = 'tde_heap'")
            node.safe_sql("ALTER TABLE test_enc4 SET ACCESS METHOD DEFAULT")
            node.safe_sql("RESET default_table_access_method")

        # test_enc5: tde_heap + truncate + reinsert
        node.safe_sql(
            "CREATE TABLE test_enc5 (id SERIAL, k VARCHAR(32), PRIMARY KEY (id))"
            " USING tde_heap"
        )
        node.safe_sql(
            "INSERT INTO test_enc5 (k) VALUES ('multitude'), ('multitudinous')"
        )
        node.safe_sql("CHECKPOINT")
        node.safe_sql("TRUNCATE test_enc5")
        node.safe_sql(
            "INSERT INTO test_enc5 (k) VALUES ('multitude'), ('multitudinous')"
        )

        # test_enc6: plain heap control (stays unencrypted)
        node.safe_sql(
            "CREATE TABLE test_enc6 (id SERIAL, k VARCHAR(32), PRIMARY KEY (id))"
            " USING heap"
        )
        node.safe_sql(
            "INSERT INTO test_enc6 (k) VALUES ('multitude'), ('multitudinous')"
        )

        enc = ["test_enc1", "test_enc2", "test_enc3", "test_enc5"]
        if has_am_default:
            enc.append("test_enc4")

        # read round-trip, then restart and re-read
        for _ in range(2):
            for t in [*enc, "test_enc6"]:
                rows = node.safe_sql(f"SELECT k FROM {t} ORDER BY id")
                assert [r[0] for r in rows] == ["multitude", "multitudinous"], t
            node.restart()

        # on-disk: encrypted tables hide the plaintext, the heap control does not
        node.safe_sql("CHECKPOINT")
        for t in enc:
            tde.assert_not_on_disk(t, "multitud")
        tde.assert_on_disk("test_enc6", "multitud")


if __name__ == "__main__":
    st.main()

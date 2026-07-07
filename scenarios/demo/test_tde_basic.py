from pathlib import Path

import stormweaver.testing as st
from conftest import TDE_DIR, requires_pg_tde

pytestmark = requires_pg_tde

WHITELIST_SQL = """
    SELECT pg_proc.oid::regprocedure::text
    FROM pg_catalog.pg_proc
        JOIN pg_catalog.pg_language ON prolang = pg_language.oid
        LEFT JOIN LATERAL aclexplode(proacl) ON TRUE
    WHERE proname LIKE 'pg_tde%' AND
        (lanname = 'c' OR prosecdef) AND
        (grantee IS NULL OR grantee = 0)
    ORDER BY 1
"""


def test_tde_basic(tmp_path: Path):
    keyring = tmp_path / "db.keys"
    with st.PgTestNode.fresh(
        TDE_DIR,
        name="tde",
        config={"shared_preload_libraries": "pg_tde"},
    ) as node:
        node.safe_sql("CREATE EXTENSION pg_tde;")

        funcs = [r[0] for r in node.safe_sql(WHITELIST_SQL)]
        assert funcs == ["pg_tde_is_encrypted(regclass)", "pg_tde_version()"]

        version = node.sql_value(
            "SELECT extversion FROM pg_extension WHERE extname = 'pg_tde'"
        )
        assert version  # exact value tracks the checkout; assert presence only

        node.expect_error(
            "principal key not configured",
            "CREATE TABLE test_enc (id SERIAL, k INTEGER, PRIMARY KEY (id))"
            " USING tde_heap;",
        )

        node.safe_sql(
            "SELECT pg_tde_add_database_key_provider_file('file-vault', $1)",
            (str(keyring),),
        )
        node.safe_sql(
            "SELECT pg_tde_create_key_using_database_key_provider"
            "('test-db-key', 'file-vault')"
        )
        node.safe_sql(
            "SELECT pg_tde_set_key_using_database_key_provider"
            "('test-db-key', 'file-vault')"
        )

        node.safe_sql(
            "CREATE TABLE test_enc (id SERIAL, k VARCHAR(32), PRIMARY KEY (id))"
            " USING tde_heap;"
        )
        node.safe_sql("INSERT INTO test_enc (k) VALUES ('foobar'), ('barfoo');")
        expected = [["1", "foobar"], ["2", "barfoo"]]
        assert node.safe_sql("SELECT * FROM test_enc ORDER BY id;") == expected

        node.restart()
        assert node.safe_sql("SELECT * FROM test_enc ORDER BY id;") == expected

        assert b"foo" not in node.read_relation_file("test_enc")

        # encrypted table can be dropped without access to the principal key
        node.stop()
        keyring.unlink()
        node.start()
        node.expect_error(
            'key "test-db-key" not found in key provider "file-vault"',
            "SELECT pg_tde_verify_key()",
        )
        node.safe_sql("DROP TABLE test_enc;")
        node.safe_sql("DROP EXTENSION pg_tde;")


if __name__ == "__main__":
    st.main()

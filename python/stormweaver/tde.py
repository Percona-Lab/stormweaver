from typing import Any


def _execute(conn: Any, query: str) -> None:
    result = conn.execute(query)
    if not result.success():
        raise RuntimeError(f"pg_tde setup failed: {query}: {result.error_message}")


def init_tde_only_for_db(conn: Any, keyring_path: str) -> None:
    """Set up per-database encryption with a file key provider."""
    _execute(conn, "CREATE EXTENSION IF NOT EXISTS pg_tde;")
    _execute(conn, "SET default_table_access_method = tde_heap;")
    _execute(
        conn,
        f"SELECT pg_tde_add_database_key_provider_file('reg_file', '{keyring_path}');",
    )
    _execute(
        conn,
        "SELECT pg_tde_create_key_using_database_key_provider("
        "'principal-key', 'reg_file');",
    )
    _execute(
        conn,
        "SELECT pg_tde_set_key_using_database_key_provider("
        "'principal-key', 'reg_file');",
    )


def init_tde_globally(conn: Any, keyring_path: str) -> None:
    """Set up global encryption incl. server key, default key, WAL encryption."""
    _execute(conn, "CREATE EXTENSION IF NOT EXISTS pg_tde;")
    _execute(conn, "SET default_table_access_method = tde_heap;")
    _execute(
        conn,
        f"SELECT pg_tde_add_global_key_provider_file('reg_file', '{keyring_path}');",
    )
    _execute(
        conn,
        "SELECT pg_tde_create_key_using_global_key_provider("
        "'server-principal-key', 'reg_file');",
    )
    _execute(
        conn,
        "SELECT pg_tde_set_key_using_global_key_provider("
        "'server-principal-key', 'reg_file');",
    )
    _execute(
        conn,
        "SELECT pg_tde_set_server_key_using_global_key_provider("
        "'server-principal-key', 'reg_file');",
    )
    _execute(
        conn,
        "SELECT pg_tde_set_default_key_using_global_key_provider("
        "'def-principal-key', 'reg_file');",
    )
    _execute(conn, "ALTER SYSTEM SET pg_tde.wal_encrypt = ON;")

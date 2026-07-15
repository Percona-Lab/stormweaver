from pathlib import Path
from typing import Any

from stormweaver.keyrings import pg_tde as tde_sql
from stormweaver.keyrings.base import Keyring
from stormweaver.keyrings.file import FileKeyring


def _execute(conn: Any, query: str) -> None:
    result = conn.execute(query)
    if not result.success():
        raise RuntimeError(f"pg_tde setup failed: {query}: {result.error_message}")


def _keyring(keyring: str | Keyring) -> Keyring:
    return FileKeyring(Path(keyring)) if isinstance(keyring, str) else keyring


def init_tde_only_for_db(conn: Any, keyring: str | Keyring) -> None:
    """Set up per-database encryption; a str is a file keyring path."""
    kr = _keyring(keyring)
    _execute(conn, "CREATE EXTENSION IF NOT EXISTS pg_tde;")
    _execute(conn, "SET default_table_access_method = tde_heap;")
    _execute(conn, tde_sql.add_provider_sql(kr, "database", "reg_file") + ";")
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


def init_tde_globally(conn: Any, keyring: str | Keyring) -> None:
    """Set up global encryption incl. server key, default key, WAL encryption."""
    kr = _keyring(keyring)
    _execute(conn, "CREATE EXTENSION IF NOT EXISTS pg_tde;")
    _execute(conn, "SET default_table_access_method = tde_heap;")
    _execute(conn, tde_sql.add_provider_sql(kr, "global", "reg_file") + ";")
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
        "SELECT pg_tde_create_key_using_global_key_provider("
        "'def-principal-key', 'reg_file');",
    )
    _execute(
        conn,
        "SELECT pg_tde_set_default_key_using_global_key_provider("
        "'def-principal-key', 'reg_file');",
    )
    _execute(conn, "ALTER SYSTEM SET pg_tde.wal_encrypt = ON;")

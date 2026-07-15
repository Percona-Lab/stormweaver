from pathlib import Path
from typing import TYPE_CHECKING, Any, cast

from stormweaver.keyrings import pg_tde as tde_sql
from stormweaver.keyrings.base import Keyring, Scope
from stormweaver.keyrings.file import FileKeyring

if TYPE_CHECKING:
    from stormweaver.testing.node import PgTestNode


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


class PgTde:
    """Node-bound pg_tde setup + verification helper.

    Wraps a running PgTestNode and a Keyring. Methods issue the pg_tde SQL the
    migrated t/ tests need; ALTER SYSTEM changes (wal_encrypt) require a
    restart the caller controls.
    """

    def __init__(self, node: PgTestNode, keyring: Keyring) -> None:
        self.node = node
        self.keyring = keyring

    # granular setup

    def create_extension(self) -> None:
        self.node.safe_sql("CREATE EXTENSION IF NOT EXISTS pg_tde")

    def add_provider(
        self, scope: str, name: str, keyring: Keyring | None = None
    ) -> None:
        self.node.safe_sql(
            tde_sql.add_provider_sql(keyring or self.keyring, cast(Scope, scope), name)
        )

    def create_key(self, key: str, provider: str, *, scope: str = "database") -> None:
        fn = f"pg_tde_create_key_using_{scope}_key_provider"
        self.node.safe_sql(f"SELECT {fn}('{key}', '{provider}')")

    def set_key(self, key: str, provider: str) -> None:
        self.node.safe_sql(
            f"SELECT pg_tde_set_key_using_database_key_provider('{key}', '{provider}')"
        )

    def set_server_key(self, key: str, provider: str) -> None:
        self.node.safe_sql(
            f"SELECT pg_tde_set_server_key_using_global_key_provider"
            f"('{key}', '{provider}')"
        )

    def set_default_key(self, key: str, provider: str) -> None:
        self.node.safe_sql(
            f"SELECT pg_tde_set_default_key_using_global_key_provider"
            f"('{key}', '{provider}')"
        )

    def enable_wal_encryption(self) -> None:
        self.node.safe_sql("ALTER SYSTEM SET pg_tde.wal_encrypt = on")

    def disable_wal_encryption(self) -> None:
        self.node.safe_sql("ALTER SYSTEM SET pg_tde.wal_encrypt = off")

    def setup(
        self,
        *,
        scope: str = "database",
        provider: str = "provider",
        key: str = "key",
        wal: bool = False,
        default_am: bool = False,
    ) -> None:
        """Compose the common dance. Caller restarts to apply wal/default_am.

        wal=True reproduces TdeRewind.configure_primary's key setup: a global
        wal-provider/wal-key server key plus a database db-provider/db-key.
        """
        self.create_extension()
        if wal:
            self.add_provider("global", "wal-provider")
            self.create_key("wal-key", "wal-provider", scope="global")
            self.set_server_key("wal-key", "wal-provider")
            provider, key, scope = "db-provider", "db-key", "database"
        self.add_provider(scope, provider)
        self.create_key(key, provider, scope=scope)
        if scope == "database":
            self.set_key(key, provider)
        else:
            self.set_server_key(key, provider)
        if wal:
            self.enable_wal_encryption()
        if default_am:
            self.node.db.add_config({"default_table_access_method": "tde_heap"})

    # inspection / verification

    def is_encrypted(self, rel: str) -> bool:
        return self.node.sql_value(f"SELECT pg_tde_is_encrypted('{rel}')") == "t"

    def verify_key(self) -> None:
        self.node.safe_sql("SELECT pg_tde_verify_key()")

    def key_info(self) -> Any:
        return self.node.safe_sql(
            "SELECT provider_id, provider_name, key_name FROM pg_tde_key_info()"
        )

    def server_key_info(self) -> Any:
        return self.node.safe_sql(
            "SELECT key_name, provider_name, provider_id FROM pg_tde_server_key_info()"
        )

    def assert_not_on_disk(self, table: str, needle: str | bytes) -> None:
        want = needle.encode() if isinstance(needle, str) else needle
        data = self.node.read_relation_file(table)
        assert want not in data, f"{needle!r} unexpectedly found on disk in {table}"

    def assert_on_disk(self, table: str, needle: str | bytes) -> None:
        want = needle.encode() if isinstance(needle, str) else needle
        data = self.node.read_relation_file(table)
        assert want in data, f"{needle!r} not found on disk in {table}"

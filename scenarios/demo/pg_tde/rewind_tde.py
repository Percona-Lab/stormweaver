from pathlib import Path

import stormweaver.testing as st
from stormweaver.keyrings.base import Keyring
from stormweaver.tde import PgTde
from stormweaver.testing.rewind import RewindDriver

# minimal grants the perl test uses so rewind_user can drive a remote rewind
REWIND_USER_SQL = """
CREATE ROLE rewind_user LOGIN;
GRANT EXECUTE ON function pg_catalog.pg_ls_dir(text, boolean, boolean) TO rewind_user;
GRANT EXECUTE ON function pg_catalog.pg_stat_file(text, boolean) TO rewind_user;
GRANT EXECUTE ON function pg_catalog.pg_read_binary_file(text) TO rewind_user;
GRANT EXECUTE ON function
    pg_catalog.pg_read_binary_file(text, bigint, bigint, boolean) TO rewind_user;
"""


class TdeRewind(RewindDriver):
    """RewindDriver specialized for pg_tde: encrypted WAL, tde_heap, pg_tde_rewind."""

    rewind_bin = "pg_tde_rewind"

    def __init__(
        self,
        install_dir: str | Path,
        *,
        cipher: str,
        keyring: Keyring,
        debug: bool = False,
    ) -> None:
        super().__init__(install_dir, debug=debug)
        self.cipher = cipher
        self.keyring = keyring

    def primary_config(self) -> dict[str, str]:
        # cipher must be set before key creation so keys use the right cipher
        return {
            "shared_preload_libraries": "pg_tde",
            "allow_in_place_tablespaces": "on",
            "wal_keep_size": "320MB",
            "pg_tde.cipher": self.cipher,
        }

    def initdb_args(self) -> list[str] | None:
        return ["--data-checksums"]

    def rewind_user(self) -> str | None:
        return "rewind_user"

    def configure_primary(self, primary: st.PgTestNode) -> None:
        tde = PgTde(primary, self.keyring)
        # global server WAL key + database key + wal_encrypt (see PgTde.setup)
        tde.setup(wal=True)
        # tde_heap default + apply the ALTER SYSTEM wal_encrypt via a restart
        primary.db.add_config({"default_table_access_method": "tde_heap"})
        primary.restart()
        primary.safe_sql(REWIND_USER_SQL)

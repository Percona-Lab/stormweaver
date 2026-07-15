import socket
import threading
from pathlib import Path

import stormweaver.testing as st
from _pgtde import TDE_DIR, requires_kmip, requires_pg_tde
from stormweaver.keyrings import open_keyring
from stormweaver.tde import PgTde

pytestmark = [requires_pg_tde, requires_kmip]


class _RejectListener:
    """Accept TCP then immediately close — trips a fast TLS/handshake failure."""

    def __enter__(self):
        self._srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._srv.bind(("127.0.0.1", 0))
        self._srv.listen(5)
        self._srv.settimeout(0.5)
        self.port = self._srv.getsockname()[1]
        self._stop = False
        self._t = threading.Thread(target=self._run, daemon=True)
        self._t.start()
        return self

    def _run(self):
        while not self._stop:
            try:
                c, _ = self._srv.accept()
                c.close()
            except OSError:
                pass

    def __exit__(self, *exc):
        self._stop = True
        self._srv.close()
        self._t.join(timeout=2)


def _read(node):
    return [r[0] for r in node.safe_sql("SELECT k FROM test_enc ORDER BY id")]


def test_kmip(tmp_path: Path):
    with open_keyring("kmip", tmp_path / "kmip") as keyring:
        # fresh-swap section below needs a stormweaver-managed service
        st.require_managed(keyring)
        with st.PgTestNode.fresh(
            TDE_DIR, name="kmip", config={"shared_preload_libraries": "pg_tde"}
        ) as node:
            tde = PgTde(node, keyring)
            tde.create_extension()
            tde.add_provider("database", "kmip-prov")
            tde.create_key("kmip-key", "kmip-prov", scope="database")
            tde.set_key("kmip-key", "kmip-prov")

            node.safe_sql(
                "CREATE TABLE test_enc (id SERIAL, k INTEGER, PRIMARY KEY (id))"
                " USING tde_heap"
            )
            node.safe_sql("INSERT INTO test_enc (k) VALUES (1), (2), (3)")
            assert _read(node) == ["1", "2", "3"]

            node.restart()  # cold cache -> KMIP re-fetch
            assert _read(node) == ["1", "2", "3"]

            # key rotation + post-rotation insert
            tde.create_key("kmip-key2", "kmip-prov", scope="database")
            tde.set_key("kmip-key2", "kmip-prov")
            node.safe_sql("INSERT INTO test_enc (k) VALUES (4), (5)")
            assert _read(node) == ["1", "2", "3", "4", "5"]

            node.restart()
            assert _read(node) == ["1", "2", "3", "4", "5"]

            # swap in a fresh, empty KMIP server on the same TLS/ports
            node.stop()
            keyring.restart_service(fresh=True)
            node.start()
            node.expect_error(
                r"not found in key provider",
                "SELECT k FROM test_enc ORDER BY id",
            )

            # dead endpoint: provider add must fail fast on the TLS handshake
            with _RejectListener() as lst:
                node.expect_error(
                    r"SSL error|BIO_do_connect|handshake|EOF|failed",
                    "SELECT pg_tde_add_database_key_provider_kmip("
                    f"'will-not-work', '127.0.0.1', {lst.port}, "
                    f"'{keyring.client_cert}', '{keyring.client_key}', "
                    f"'{keyring.ca_cert}')",
                )


if __name__ == "__main__":
    st.main()

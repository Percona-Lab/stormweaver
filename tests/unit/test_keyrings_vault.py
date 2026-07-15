import socket

import pytest
from stormweaver.keyrings import vault


def test_find_binary_env_override(monkeypatch, tmp_path):
    fake = tmp_path / "bao"
    fake.write_text("#!/bin/sh\n")
    fake.chmod(0o755)
    monkeypatch.setenv("OPENBAO_BIN", str(fake))
    assert vault.find_binary({}) == str(fake)
    monkeypatch.delenv("OPENBAO_BIN")
    monkeypatch.setattr(vault.shutil, "which", lambda _: None)
    assert vault.find_binary({}) is None


def test_open_external_missing_keys(tmp_path):
    with pytest.raises(
        RuntimeError, match="external mode missing: token_file, ca_cert"
    ):
        vault.open_external({"url": "https://h:1"}, tmp_path)


def test_open_external_ok(tmp_path):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        s.listen(1)
        port = s.getsockname()[1]
        k = vault.open_external(
            {
                "url": f"https://127.0.0.1:{port}",
                "token_file": "/t",
                "ca_cert": "/ca.pem",
                "namespace": "ns1",
            },
            tmp_path,
        )
    assert k.url == f"https://127.0.0.1:{port}"
    assert k.mount_path == "secret"
    assert k.namespace == "ns1"
    assert not k.managed
    k.close()

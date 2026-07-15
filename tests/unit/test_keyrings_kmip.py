import socket

import pytest
from stormweaver.keyrings import kmip
from stormweaver.keyrings.certs import gen_kmip_certs


def test_gen_kmip_certs(tmp_path):
    gen_kmip_certs(tmp_path / "certs")
    for name in (
        "ca.pem",
        "server.pem",
        "server.key",
        "server.p12",
        "client.pem",
        "client.key",
    ):
        f = tmp_path / "certs" / name
        assert f.exists() and f.stat().st_size > 0, name


def test_kms_toml_render():
    toml = kmip._kms_toml("/data", "0.0.0.0", 5696, 9998)
    assert 'sqlite_path   = "/data/db"' in toml
    assert 'tls_p12_file         = "/data/server.p12"' in toml
    assert "socket_server_port     = 5696" in toml
    assert "port     = 9998" in toml
    assert 'hostname = "0.0.0.0"' in toml


def test_find_binary_env_override(monkeypatch, tmp_path):
    fake = tmp_path / "cosmian_kms"
    fake.write_text("#!/bin/sh\n")
    fake.chmod(0o755)
    monkeypatch.setenv("COSMIAN_KMS_BIN", str(fake))
    assert kmip.find_binary({}) == str(fake)
    monkeypatch.setenv("COSMIAN_KMS_BIN", str(tmp_path / "missing"))
    assert kmip.find_binary({}) is None


def test_open_external_missing_keys(tmp_path):
    with pytest.raises(RuntimeError, match="external mode missing: host, port"):
        kmip.open_external(
            {"client_cert": "c", "client_key": "k", "ca_cert": "a"}, tmp_path
        )


def test_open_external_ok(tmp_path):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        s.listen(1)
        port = s.getsockname()[1]
        k = kmip.open_external(
            {
                "host": "127.0.0.1",
                "port": port,
                "client_cert": "/c.pem",
                "client_key": "/c.key",
                "ca_cert": "/ca.pem",
            },
            tmp_path,
        )
    assert k.host == "127.0.0.1"
    assert k.port == port
    assert not k.managed
    k.close()

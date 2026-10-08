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


def test_cli_sets_env_and_runs(tmp_path):
    tok = tmp_path / "token"
    tok.write_text("root\n")
    k = vault.VaultKeyring(
        url="https://127.0.0.1:1",
        mount_path="secret",
        token_file=tok,
        ca_cert=tmp_path / "ca.pem",
        namespace="pgns",
        binary="/usr/bin/env",
    )
    cp = k.cli(
        "sh", "-c", 'echo "$VAULT_ADDR|$VAULT_CACERT|$VAULT_TOKEN|$VAULT_NAMESPACE"'
    )
    assert cp.returncode == 0
    assert cp.stdout.strip() == f"https://127.0.0.1:1|{tmp_path / 'ca.pem'}|root|pgns"


def test_cli_failure_raises(tmp_path):
    tok = tmp_path / "token"
    tok.write_text("root")
    k = vault.VaultKeyring(
        url="https://h:1",
        mount_path="m",
        token_file=tok,
        ca_cert=tmp_path / "ca",
        binary="/bin/false",
    )
    with pytest.raises(RuntimeError, match="failed"):
        k.cli("anything")


def test_cli_without_binary_raises(tmp_path):
    k = vault.VaultKeyring(
        url="https://h:1",
        mount_path="m",
        token_file=tmp_path / "t",
        ca_cert=tmp_path / "ca",
    )
    with pytest.raises(RuntimeError, match="CLI not available"):
        k.cli("status")


def test_cli_scrubs_inherited_vault_and_bao_env(tmp_path, monkeypatch):
    monkeypatch.setenv("BAO_ADDR", "leaked")
    monkeypatch.setenv("VAULT_NAMESPACE", "leaked")
    monkeypatch.setenv("VAULT_SKIP_VERIFY", "1")
    tok = tmp_path / "token"
    tok.write_text("root")
    k = vault.VaultKeyring(
        url="https://h:1",
        mount_path="m",
        token_file=tok,
        ca_cert=tmp_path / "ca",
        binary="/usr/bin/env",
    )
    cp = k.cli(
        "sh",
        "-c",
        'echo "$BAO_ADDR|$VAULT_ADDR|[$VAULT_NAMESPACE]|[$BAO_NAMESPACE]'
        '|[$VAULT_SKIP_VERIFY]"',
    )
    assert cp.stdout.strip() == "https://h:1|https://h:1|[]|[]|[]"

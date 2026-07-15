import pytest
import stormweaver.keyrings as kr


def _open_or_skip(kind, mode, tmp_path):
    cfg = {kind: {"provision": mode}}
    if kr.resolve_provision(kind, cfg[kind]) is None:
        pytest.skip(f"{kind} not available via {mode}")
    return kr.open_keyring(kind, tmp_path / "kr", cfg)


@pytest.mark.parametrize("mode", ["executable", "container"])
def test_vault_service_starts(mode, tmp_path):
    with _open_or_skip("vault", mode, tmp_path) as k:
        assert k.managed
        assert k.token_file.read_text()
        assert k.ca_cert.exists()
        assert k.url.startswith("https://127.0.0.1:")


@pytest.mark.parametrize("mode", ["executable", "container"])
def test_kmip_service_starts_and_fresh_restart(mode, tmp_path):
    with _open_or_skip("kmip", mode, tmp_path) as k:
        assert k.managed
        assert k.client_cert.exists()
        assert k.ca_cert.exists()
        k.restart_service(fresh=True)  # must come back ready


def test_file_no_lifecycle(tmp_path):
    with kr.open_keyring("file", tmp_path, {}) as k:
        assert not k.managed
        with pytest.raises(RuntimeError):
            k.restart_service()

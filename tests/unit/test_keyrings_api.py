import pytest
import stormweaver.keyrings as kr
from stormweaver.keyrings import kmip, provision, vault
from stormweaver.keyrings.file import FileKeyring


@pytest.fixture(autouse=True)
def _isolate(monkeypatch):
    monkeypatch.delenv("STORMWEAVER_KEYRINGS", raising=False)
    monkeypatch.delenv("STORMWEAVER_CONFIG", raising=False)
    provision.detect_runtime.cache_clear()
    yield
    provision.detect_runtime.cache_clear()


def _no_services(monkeypatch):
    monkeypatch.setattr(vault, "find_binary", lambda cfg: None)
    monkeypatch.setattr(kmip, "find_binary", lambda cfg: None)
    monkeypatch.setattr(provision.shutil, "which", lambda _: None)


def test_resolve_file_always():
    assert kr.resolve_provision("file", {}) == "local"


def test_resolve_auto_prefers_executable(monkeypatch):
    monkeypatch.setattr(vault, "find_binary", lambda cfg: "/fake/bao")
    assert kr.resolve_provision("vault", {}) == "executable"


def test_resolve_auto_unavailable(monkeypatch):
    _no_services(monkeypatch)
    assert kr.resolve_provision("vault", {}) is None
    assert kr.resolve_provision("kmip", {}) is None


def test_resolve_external_only_explicit(monkeypatch):
    _no_services(monkeypatch)
    assert kr.resolve_provision("vault", {"provision": "external"}) == "external"


def test_resolve_unknown_mode():
    with pytest.raises(ValueError, match="unknown provision mode"):
        kr.resolve_provision("vault", {"provision": "magic"})


def test_available_and_selected(monkeypatch):
    _no_services(monkeypatch)
    assert kr.available_keyrings({}) == ["file"]
    monkeypatch.setattr(kmip, "find_binary", lambda cfg: "/fake/kms")
    assert kr.available_keyrings({}) == ["file", "kmip"]
    assert kr.selected_keyrings({}) == ["file", "kmip"]


def test_selected_forced(monkeypatch):
    _no_services(monkeypatch)
    monkeypatch.setattr(kmip, "find_binary", lambda cfg: "/fake/kms")
    monkeypatch.setenv("STORMWEAVER_KEYRINGS", "kmip")
    assert kr.selected_keyrings({}) == ["kmip"]


def test_selected_forced_unavailable(monkeypatch):
    _no_services(monkeypatch)
    monkeypatch.setenv("STORMWEAVER_KEYRINGS", "vault")
    with pytest.raises(RuntimeError, match="not available: vault"):
        kr.selected_keyrings({})


def test_selected_forced_unknown(monkeypatch):
    monkeypatch.setenv("STORMWEAVER_KEYRINGS", "file,pkcs11")
    with pytest.raises(RuntimeError, match="unknown keyring"):
        kr.selected_keyrings({})


def test_open_file_keyring(tmp_path):
    with kr.open_keyring("file", tmp_path, {}) as k:
        assert isinstance(k, FileKeyring)
        assert k.path == tmp_path / "keyring.per"


def test_open_unavailable_raises(monkeypatch, tmp_path):
    _no_services(monkeypatch)
    with pytest.raises(RuntimeError, match="not available"):
        kr.open_keyring("vault", tmp_path, {})


def test_load_keyring_config(monkeypatch, tmp_path):
    cfg = tmp_path / "sw.toml"
    cfg.write_text('[keyring.kmip]\nprovision = "container"\n')
    monkeypatch.setenv("STORMWEAVER_CONFIG", str(cfg))
    assert kr.load_keyring_config()["kmip"]["provision"] == "container"
    monkeypatch.setenv("STORMWEAVER_CONFIG", str(tmp_path / "missing.toml"))
    assert kr.load_keyring_config() == {}


def test_keyring_params(monkeypatch):
    import stormweaver.testing as st

    _no_services(monkeypatch)
    monkeypatch.setattr(kmip, "find_binary", lambda cfg: "/fake/kms")
    monkeypatch.setattr(kr, "load_keyring_config", lambda path=None: {})
    params = st.keyring_params()
    assert [p.values[0] for p in params] == ["file", "vault", "kmip"]
    assert not params[0].marks
    assert params[1].marks[0].name == "skip"
    assert not params[2].marks


def test_keyring_params_forced(monkeypatch):
    import stormweaver.testing as st

    _no_services(monkeypatch)
    monkeypatch.setattr(kmip, "find_binary", lambda cfg: "/fake/kms")
    monkeypatch.setattr(kr, "load_keyring_config", lambda path=None: {})
    monkeypatch.setenv("STORMWEAVER_KEYRINGS", "kmip")
    params = st.keyring_params()
    assert [p.values[0] for p in params] == ["kmip"]


def test_require_managed_skips(tmp_path):
    import stormweaver.testing as st

    with (
        kr.open_keyring("file", tmp_path, {}) as k,
        pytest.raises(pytest.skip.Exception),
    ):
        st.require_managed(k)

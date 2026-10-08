import socket
from pathlib import Path

import pytest
import stormweaver as sw


def _config(tmp_path, body):
    cfg_file = tmp_path / "sw.toml"
    cfg_file.write_text(body)
    return sw.Config.load(str(cfg_file))


def test_free_port_within_range(tmp_path):
    cfg = _config(tmp_path, "[default]\nport_start = 26200\nport_end = 26299\n")
    port = cfg.free_port()
    assert 26200 <= port <= 26299


def test_free_port_no_repeat(tmp_path):
    cfg = _config(tmp_path, "[default]\nport_start = 26300\nport_end = 26399\n")
    ports = {cfg.free_port() for _ in range(10)}
    assert len(ports) == 10


def test_free_port_skips_occupied(tmp_path):
    cfg = _config(tmp_path, "[default]\nport_start = 26400\nport_end = 26401\n")
    with socket.socket() as s:
        s.bind(("127.0.0.1", 26400))
        s.listen(1)
        assert cfg.free_port() == 26401


def test_free_port_exhausted_range(tmp_path):
    cfg = _config(tmp_path, "[default]\nport_start = 26500\nport_end = 26500\n")
    with socket.socket() as s:
        s.bind(("127.0.0.1", 26500))
        s.listen(1)
        with pytest.raises(RuntimeError):
            cfg.free_port()


def test_config_load(tmp_path):
    cfg_file = tmp_path / "sw.toml"
    cfg_file.write_text('[default]\npgroot = "/opt/pg"\nport_start = 26000\n')
    cfg = sw.Config.load(str(cfg_file))
    assert cfg.pgroot == "/opt/pg"
    assert cfg.port_start == 26000


def test_config_defaults(tmp_path):
    cfg_file = tmp_path / "sw.toml"
    cfg_file.write_text("")
    cfg = sw.Config.load(str(cfg_file))
    assert cfg.pgroot == ""
    assert cfg.port_start == 15432
    assert cfg.port_end == 15531


def test_config_datadir_per_name(tmp_path):
    cfg_file = tmp_path / "sw.toml"
    cfg_file.write_text('[default]\npgroot = "/opt/pg"\n')
    cfg = sw.Config.load(str(cfg_file))
    assert cfg.datadir("primary") != cfg.datadir("replica")


def test_alloc_port_bindable():
    from stormweaver.config import alloc_port

    port = alloc_port(26600, 26700)
    assert 26600 <= port < 26700
    with socket.socket() as s:
        s.bind(("127.0.0.1", port))


def test_config_keyrings_section(tmp_path):
    cfg = _config(
        tmp_path,
        '[keyring.vault]\nprovision = "external"\nurl = "https://h:8200"\n',
    )
    assert cfg.keyrings["vault"]["provision"] == "external"
    assert cfg.keyrings["vault"]["url"] == "https://h:8200"


def test_config_keyrings_default_empty(tmp_path):
    cfg = _config(tmp_path, "[default]\n")
    assert cfg.keyrings == {}


def test_resolve_explicit_wins(tmp_path, monkeypatch):
    monkeypatch.setenv("STORMWEAVER_CONFIG", str(tmp_path / "env.toml"))
    assert sw.config.resolve_config_path("x.toml") == Path("x.toml")


def test_resolve_env_before_cwd(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    (tmp_path / "config").mkdir()
    (tmp_path / "config" / "stormweaver.toml").write_text("")
    monkeypatch.setenv("STORMWEAVER_CONFIG", "/nonexistent/env.toml")
    assert sw.config.resolve_config_path(None) == Path("/nonexistent/env.toml")


def test_resolve_cwd_before_etc(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    monkeypatch.delenv("STORMWEAVER_CONFIG", raising=False)
    (tmp_path / "config").mkdir()
    (tmp_path / "config" / "stormweaver.toml").write_text("")
    assert sw.config.resolve_config_path(None) == Path("config/stormweaver.toml")


def test_resolve_etc_fallback(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    monkeypatch.delenv("STORMWEAVER_CONFIG", raising=False)
    etc = tmp_path / "etc.toml"
    etc.write_text("")
    monkeypatch.setattr(
        sw.config, "DEFAULT_CONFIG_PATHS", (Path("config/stormweaver.toml"), etc)
    )
    assert sw.config.resolve_config_path(None) == etc


def test_load_defaults_without_any_file(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    monkeypatch.delenv("STORMWEAVER_CONFIG", raising=False)
    monkeypatch.setattr(sw.config, "DEFAULT_CONFIG_PATHS", (tmp_path / "none.toml",))
    cfg = sw.Config.load(None)
    assert cfg.port_start == 15432


def test_load_explicit_missing_raises(tmp_path):
    with pytest.raises(FileNotFoundError):
        sw.Config.load(str(tmp_path / "missing.toml"))

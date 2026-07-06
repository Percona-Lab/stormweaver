import socket

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

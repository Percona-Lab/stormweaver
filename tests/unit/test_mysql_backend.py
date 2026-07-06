import subprocess

import stormweaver as sw


class FakeCompleted:
    def __init__(self, returncode=0, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def test_mysql_is_backend(tmp_path):
    my = sw.MySQL(
        install_dir="/opt/mysql", datadir=str(tmp_path / "d"), port=23306, init=False
    )
    assert isinstance(my, sw.DatabaseBackend)


def test_initialize_builds_mysqld_command_and_writes_my_cnf(tmp_path, monkeypatch):
    install_dir = tmp_path / "install"
    (install_dir / "bin").mkdir(parents=True)
    (install_dir / "bin" / "mysqld").touch()
    datadir = tmp_path / "d"

    calls = []

    def fake_run(cmd, **kwargs):
        calls.append(cmd)
        return FakeCompleted(returncode=0)

    monkeypatch.setattr(subprocess, "run", fake_run)
    my = sw.MySQL(
        install_dir=str(install_dir), datadir=str(datadir), port=23306, init=False
    )
    my.initialize()

    assert len(calls) == 1
    cmd = calls[0]
    assert str(install_dir / "bin" / "mysqld") in cmd
    assert "--initialize-insecure" in cmd
    assert any(str(datadir / "data") in str(part) for part in cmd)

    conf_file = datadir / "my.cnf"
    content = conf_file.read_text()
    lines = content.splitlines()
    assert lines[0] == "[mysqld]"
    assert f"datadir = {datadir / 'data'}" in content
    assert "port = 23306" in content
    assert f"socket = {datadir / 'mysql.sock'}" in content
    assert "log-error" in content
    assert "pid-file" in content


def test_bin_prefers_bin_over_sbin(tmp_path):
    install_dir = tmp_path / "install"
    (install_dir / "bin").mkdir(parents=True)
    (install_dir / "sbin").mkdir(parents=True)
    (install_dir / "bin" / "mysqld").touch()
    (install_dir / "sbin" / "mysqld").touch()

    my = sw.MySQL(install_dir=str(install_dir), datadir=str(tmp_path / "d"), init=False)
    assert my._bin("mysqld") == str(install_dir / "bin" / "mysqld")


def test_bin_falls_back_to_sbin(tmp_path):
    install_dir = tmp_path / "install"
    (install_dir / "sbin").mkdir(parents=True)
    (install_dir / "sbin" / "mysqld").touch()

    my = sw.MySQL(install_dir=str(install_dir), datadir=str(tmp_path / "d"), init=False)
    assert my._bin("mysqld") == str(install_dir / "sbin" / "mysqld")


def test_bin_falls_back_to_bin_string_when_neither_exists(tmp_path):
    install_dir = tmp_path / "install"
    my = sw.MySQL(install_dir=str(install_dir), datadir=str(tmp_path / "d"), init=False)
    assert my._bin("mysqld") == str(install_dir / "bin" / "mysqld")


def test_add_config_appends_under_header(tmp_path):
    datadir = tmp_path / "d"
    datadir.mkdir()
    my = sw.MySQL(
        install_dir="/opt/mysql", datadir=str(datadir), port=23306, init=False
    )
    conf_file = datadir / "my.cnf"
    conf_file.write_text("[mysqld]\n")

    my.add_config({"key1": "val1", "key2": "val2"})

    content = conf_file.read_text()
    lines = content.splitlines()
    assert lines[0] == "[mysqld]"
    assert "key1 = val1" in content
    assert "key2 = val2" in content


def test_connection_params(tmp_path):
    my = sw.MySQL(
        install_dir="/opt/mysql", datadir=str(tmp_path / "d"), port=23306, init=False
    )
    params = my.connection_params("mydb")
    assert params["host"] == "127.0.0.1"
    assert params["port"] == 23306
    assert params["dbname"] == "mydb"
    assert params["user"] == "root"


def test_stop_when_never_started_does_not_crash(tmp_path, monkeypatch):
    my = sw.MySQL(
        install_dir="/opt/mysql", datadir=str(tmp_path / "d"), port=23306, init=False
    )

    def fake_run(cmd, **kwargs):
        return FakeCompleted(returncode=1, stderr="no server")

    monkeypatch.setattr(subprocess, "run", fake_run)
    my.stop()
    assert my._proc is None


def test_is_running_false_initially(tmp_path):
    my = sw.MySQL(
        install_dir="/opt/mysql", datadir=str(tmp_path / "d"), port=23306, init=False
    )
    assert my.is_running() is False

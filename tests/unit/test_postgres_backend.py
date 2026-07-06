import subprocess

import stormweaver as sw


class FakeCompleted:
    def __init__(self, returncode=0, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def test_postgres_is_backend(tmp_path):
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    assert isinstance(pg, sw.DatabaseBackend)


def test_start_records_install_and_datadir_in_command(tmp_path, monkeypatch):
    datadir = tmp_path / "d"
    datadir.mkdir()
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(datadir), port=26100, init=False
    )

    calls = []

    def fake_run(cmd, **kwargs):
        calls.append(cmd)
        return FakeCompleted(returncode=0)

    monkeypatch.setattr(subprocess, "run", fake_run)
    pg.start()

    assert len(calls) == 1
    cmd = calls[0]
    assert any("/opt/pg" in str(part) for part in cmd)
    assert any(str(datadir) in str(part) for part in cmd)


def test_is_running_reflects_pid_liveness(tmp_path, monkeypatch):
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    assert pg.is_running() is False

    import os

    pg._pid = os.getpid()
    assert pg.is_running() is True

    pg._pid = 2**30  # extremely unlikely to be a real pid
    assert pg.is_running() is False


def test_kill_sends_sigkill(tmp_path, monkeypatch):
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    pg._pid = 4242

    killed = {}

    def fake_kill(pid, sig):
        killed["pid"] = pid
        killed["sig"] = sig

    import os

    monkeypatch.setattr(os, "kill", fake_kill)
    pg.kill()

    import signal

    assert killed == {"pid": 4242, "sig": signal.SIGKILL}
    assert pg._pid is None


def test_connection_params(monkeypatch):
    monkeypatch.setenv("PGUSER", "tester")
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir="/tmp/whatever", port=26100, init=False
    )
    params = pg.connection_params("mydb")
    assert params["port"] == 26100
    assert params["dbname"] == "mydb"
    assert params["user"] == "tester"
    assert params["host"] == "localhost"

import subprocess

import pytest
import stormweaver as sw


class FakeCompleted:
    def __init__(self, returncode=0, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


class FakeProc:
    def __init__(self, cmd=None, pid=4242):
        self.cmd = cmd
        self.pid = pid
        self.returncode = None

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        if self.returncode is None:
            self.returncode = 0
        return self.returncode

    def kill(self):
        self.returncode = -9


def test_postgres_is_backend(tmp_path):
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    assert isinstance(pg, sw.DatabaseBackend)


def test_start_spawns_postgres_directly(tmp_path, monkeypatch):
    datadir = tmp_path / "d"
    datadir.mkdir()
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(datadir), port=26100, init=False
    )

    spawned = []

    def fake_popen(cmd, **kwargs):
        spawned.append(cmd)
        return FakeProc(cmd)

    monkeypatch.setattr(subprocess, "Popen", fake_popen)
    pg.start()

    assert len(spawned) == 1
    cmd = spawned[0]
    assert cmd[0] == "/opt/pg/bin/postgres"
    assert cmd[1:] == ["-D", str(datadir)]
    assert pg.is_running()


def test_is_running_reflects_proc_state(tmp_path):
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    assert pg.is_running() is False
    pg._proc = FakeProc()
    assert pg.is_running() is True
    pg._proc.returncode = 0
    assert pg.is_running() is False


def test_kill_sends_sigkill_to_server_pid(tmp_path, monkeypatch):
    import os
    import signal

    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    pg._proc = FakeProc(pid=4242)
    pg._session = 1

    killed = {}
    monkeypatch.setattr(os, "kill", lambda pid, sig: killed.update(pid=pid, sig=sig))
    pg.kill()

    assert killed == {"pid": 4242, "sig": signal.SIGKILL}
    assert pg._proc is None


def test_kill_wrapped_uses_pidfile(tmp_path, monkeypatch):
    import os

    datadir = tmp_path / "d"
    datadir.mkdir()
    pg = sw.Postgres(
        install_dir="/opt/pg",
        datadir=str(datadir),
        port=26100,
        init=False,
        wrapper=sw.ExecPrefixWrapper(["env"]),
    )
    (datadir / "postmaster.pid").write_text("2222\nmore lines\n")
    pg._proc = FakeProc(pid=1111)
    pg._session = 1

    killed = {}
    monkeypatch.setattr(os, "kill", lambda pid, sig: killed.update(pid=pid, sig=sig))
    pg.kill()
    assert killed["pid"] == 2222


def test_session_classification(tmp_path):
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    assert pg._session_result("stop", 0) == "clean"
    assert pg._session_result("stop", 1) == "crashed"
    assert pg._session_result("kill", -9) == "killed"
    assert pg._session_result(None, -11) == "crashed"


def test_stop_fires_on_session_end(tmp_path, monkeypatch):
    from stormweaver.wrappers import ServerWrapper

    class RecordingWrapper(ServerWrapper):
        def __init__(self):
            self.ended = []

        def on_session_end(self, ctx, crashed):
            self.ended.append((ctx.session, crashed))

    rw = RecordingWrapper()
    pg = sw.Postgres(
        install_dir="/opt/pg",
        datadir=str(tmp_path / "d"),
        port=26100,
        init=False,
        wrapper=rw,
    )
    pg._proc = FakeProc()
    pg._session = 1
    monkeypatch.setattr(subprocess, "run", lambda *a, **k: FakeCompleted(returncode=0))
    pg.stop()
    assert rw.ended == [(1, False)]


def test_timeout_scaling(tmp_path):
    from stormweaver.wrappers import ExecPrefixWrapper

    pg = sw.Postgres(
        install_dir="/opt/pg",
        datadir=str(tmp_path / "d"),
        port=26100,
        init=False,
        wrapper=ExecPrefixWrapper(["env"], time_multiplier=3.0),
    )
    assert pg._timeout(10.0) == 30.0


def test_reap_force_kills_stubborn_proc(tmp_path):
    class StubbornProc:
        pid = 4242

        def __init__(self):
            self.returncode = None
            self.killed = False

        def wait(self, timeout=None):
            if not self.killed:
                raise subprocess.TimeoutExpired("cmd", timeout)
            self.returncode = -9
            return self.returncode

        def kill(self):
            self.killed = True

    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    proc = StubbornProc()
    pg._proc = proc
    pg._session = 1
    pg._reap("stop", 0.1)
    assert proc.killed
    assert pg._proc is None


def test_reap_records_crashed_outcome(tmp_path, monkeypatch):
    from stormweaver import log as swlog

    run_dir = tmp_path / "run"
    run_dir.mkdir()
    monkeypatch.setattr(swlog, "_run_dir", run_dir)

    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    proc = FakeProc()
    proc.returncode = -11
    pg._proc = proc
    pg._session = 2
    pg._reap(None)

    content = (run_dir / "outcome").read_text()
    assert "node=" in content
    assert "session=2" in content
    assert "result=crashed" in content
    assert "exit=SIGSEGV" in content


def test_wait_ready_exits_early_when_server_dies(tmp_path, monkeypatch):
    import time

    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    proc = FakeProc()
    proc.returncode = 1
    pg._proc = proc
    monkeypatch.setattr(pg, "is_ready", lambda: False)

    start = time.monotonic()
    assert pg.wait_ready(timeout=5) is False
    assert time.monotonic() - start < 2


def test_kill_wrapped_no_pidfile_falls_back_to_proc_pid(tmp_path, monkeypatch):
    import os

    datadir = tmp_path / "d"
    datadir.mkdir()
    pg = sw.Postgres(
        install_dir="/opt/pg",
        datadir=str(datadir),
        port=26100,
        init=False,
        wrapper=sw.ExecPrefixWrapper(["env"]),
    )
    pg._proc = FakeProc(pid=1111)
    pg._session = 1

    killed = {}
    monkeypatch.setattr(os, "kill", lambda pid, sig: killed.update(pid=pid, sig=sig))
    pg.kill()
    assert killed["pid"] == 1111
    assert pg._proc is None


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


def test_basebackup_incremental_flag(tmp_path, monkeypatch):
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    calls = []

    def fake_run(cmd, **kwargs):
        calls.append(cmd)
        return FakeCompleted(returncode=0)

    monkeypatch.setattr(subprocess, "run", fake_run)
    pg.basebackup(str(tmp_path / "b1"), incremental="b0/backup_manifest")

    cmd = calls[0]
    idx = cmd.index("-i")
    assert cmd[idx + 1] == "b0/backup_manifest"


def test_combinebackup_command(tmp_path, monkeypatch):
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    calls = []

    def fake_run(cmd, **kwargs):
        calls.append(cmd)
        return FakeCompleted(returncode=0)

    monkeypatch.setattr(subprocess, "run", fake_run)
    pg.combinebackup(["b0", "b1"], str(tmp_path / "out"))

    cmd = calls[0]
    assert cmd[0].endswith("pg_combinebackup")
    assert cmd[1:3] == ["b0", "b1"]
    assert cmd[cmd.index("-o") + 1] == str(tmp_path / "out")


def test_combinebackup_raises_on_failure(tmp_path, monkeypatch):
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    monkeypatch.setattr(
        subprocess, "run", lambda *a, **k: FakeCompleted(returncode=1, stderr="boom")
    )
    with pytest.raises(RuntimeError, match="pg_combinebackup"):
        pg.combinebackup(["b0"], str(tmp_path / "out"))


def test_server_log_path_is_public(tmp_path):
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    assert pg.server_log_path.name.startswith("server-")

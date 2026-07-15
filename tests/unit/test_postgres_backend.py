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


def test_initdb_args_forwarded(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(
        subprocess, "run", lambda cmd, **k: calls.append(cmd) or FakeCompleted()
    )
    sw.Postgres(
        install_dir="/opt/pg",
        datadir=str(tmp_path / "d"),
        port=26100,
        init=True,
        initdb_args=["--data-checksums"],
    )
    initdb_cmd = calls[0]
    assert initdb_cmd[0].endswith("initdb")
    assert "--data-checksums" in initdb_cmd


def test_initdb_default_argv_unchanged(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(
        subprocess, "run", lambda cmd, **k: calls.append(cmd) or FakeCompleted()
    )
    sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=True
    )
    initdb_cmd = calls[0]
    assert initdb_cmd == ["/opt/pg/bin/initdb", "-D", str(tmp_path / "d"), "--no-sync"]


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


def test_stop_default_mode_is_fast(tmp_path, monkeypatch):
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    pg._proc = FakeProc()
    pg._session = 1
    calls = []
    monkeypatch.setattr(
        subprocess, "run", lambda cmd, **k: calls.append(cmd) or FakeCompleted()
    )
    pg.stop()
    cmd = calls[0]
    assert cmd[cmd.index("-m") + 1] == "fast"


def test_stop_immediate_mode(tmp_path, monkeypatch):
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    pg._proc = FakeProc()
    pg._session = 1
    calls = []
    monkeypatch.setattr(
        subprocess, "run", lambda cmd, **k: calls.append(cmd) or FakeCompleted()
    )
    pg.stop(mode="immediate")
    cmd = calls[0]
    assert cmd[cmd.index("-m") + 1] == "immediate"


def test_stop_rejects_bad_mode(tmp_path):
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    pg._proc = FakeProc()
    with pytest.raises(ValueError):
        pg.stop(mode="bogus")


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


def test_archive_dir_default_and_override(tmp_path):
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(tmp_path / "d"), port=26100, init=False
    )
    assert pg.archive_dir == tmp_path / "d_archive"

    pg2 = sw.Postgres(
        install_dir="/opt/pg",
        datadir=str(tmp_path / "d"),
        port=26100,
        init=False,
        archive_dir=str(tmp_path / "arch"),
    )
    assert pg2.archive_dir == tmp_path / "arch"


def test_enable_archiving_creates_dir_and_config(tmp_path):
    datadir = tmp_path / "d"
    datadir.mkdir()
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(datadir), port=26100, init=False
    )
    pg.enable_archiving()
    assert pg.archive_dir.is_dir()
    assert (pg.archive_dir.stat().st_mode & 0o777) == 0o700
    conf = (datadir / "postgresql.conf").read_text()
    assert "archive_mode = 'on'" in conf
    assert f'''archive_command = 'cp "%p" "{pg.archive_dir.resolve()}/%f"''' in conf


def test_set_signal(tmp_path):
    datadir = tmp_path / "d"
    datadir.mkdir()
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(datadir), port=26100, init=False
    )
    pg.set_signal("recovery")
    assert (datadir / "recovery.signal").exists()
    pg.set_signal("standby")
    assert (datadir / "standby.signal").exists()
    with pytest.raises(ValueError, match="bogus"):
        pg.set_signal("bogus")


def test_enable_restoring_missing_archive_raises(tmp_path):
    datadir = tmp_path / "d"
    datadir.mkdir()
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(datadir), port=26100, init=False
    )
    with pytest.raises(RuntimeError, match="archive"):
        pg.enable_restoring(tmp_path / "nonexistent")


def test_enable_restoring_from_source_node(tmp_path):
    src_dir = tmp_path / "src"
    src_dir.mkdir()
    source = sw.Postgres(
        install_dir="/opt/pg", datadir=str(src_dir), port=26100, init=False
    )
    source.archive_dir.mkdir()

    datadir = tmp_path / "d"
    datadir.mkdir()
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(datadir), port=26101, init=False
    )
    pg.enable_restoring(source, signal="recovery")
    conf = (datadir / "postgresql.conf").read_text()
    assert f'''restore_command = 'cp "{source.archive_dir.resolve()}/%f" "%p"''' in conf
    assert (datadir / "recovery.signal").exists()


def test_move_wal_to_archive(tmp_path):
    datadir = tmp_path / "d"
    wal = datadir / "pg_wal"
    (wal / "archive_status").mkdir(parents=True)
    (wal / "000000010000000000000001").write_text("w1")
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(datadir), port=26100, init=False
    )
    pg.archive_dir.mkdir()
    (pg.archive_dir / "existing").write_text("old")

    pg.move_wal_to_archive()

    assert (pg.archive_dir / "000000010000000000000001").read_text() == "w1"
    assert (pg.archive_dir / "existing").read_text() == "old"
    assert wal.is_dir() and not any(wal.iterdir())
    assert (wal.stat().st_mode & 0o777) == 0o700
    assert (pg.archive_dir.stat().st_mode & 0o777) == 0o700


def test_preserve_config_restores_content_and_mode(tmp_path):
    datadir = tmp_path / "d"
    datadir.mkdir()
    conf = datadir / "postgresql.conf"
    conf.write_text("original = 'yes'\n")
    conf.chmod(0o640)
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(datadir), port=26100, init=False
    )
    with pg.preserve_config() as saved:
        assert saved.read_text() == "original = 'yes'\n"
        assert saved != conf
        conf.write_text("clobbered by rewind\n")
    assert conf.read_text() == "original = 'yes'\n"
    assert (conf.stat().st_mode & 0o777) == 0o640
    assert not saved.exists()


def test_preserve_config_restores_on_exception(tmp_path):
    datadir = tmp_path / "d"
    datadir.mkdir()
    conf = datadir / "postgresql.conf"
    conf.write_text("original\n")
    pg = sw.Postgres(
        install_dir="/opt/pg", datadir=str(datadir), port=26100, init=False
    )
    with pytest.raises(RuntimeError, match="boom"), pg.preserve_config():
        conf.write_text("clobbered\n")
        raise RuntimeError("boom")
    assert conf.read_text() == "original\n"

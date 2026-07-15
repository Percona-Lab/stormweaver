import socket
import subprocess

import pytest
from stormweaver.keyrings import provision
from stormweaver.keyrings.provision import (
    ContainerService,
    ProcessService,
    check_reachable,
    detect_runtime,
    file_ready,
)


def _sleeper(tmp_path, **kw):
    marker = tmp_path / "ready"
    return marker, ProcessService(
        ["/bin/sh", "-c", f"echo ok > {marker}; exec sleep 60"],
        what="fake service",
        log_path=tmp_path / "svc.log",
        ready=file_ready(marker),
        **kw,
    )


def test_process_service_lifecycle(tmp_path):
    marker, svc = _sleeper(tmp_path)
    svc.start()
    assert marker.exists()
    svc.stop()
    svc.stop()  # idempotent


def test_process_service_restart_fresh(tmp_path):
    state = tmp_path / "state"
    state.write_text("data")
    marker, svc = _sleeper(
        tmp_path, fresh_paths=[state], pre_start=[tmp_path / "ready"]
    )
    svc.start()
    svc.restart(fresh=True)
    assert not state.exists()
    assert marker.exists()  # recreated by the new process
    svc.stop()


def test_process_service_ready_timeout(tmp_path):
    svc = ProcessService(
        ["/bin/sh", "-c", "echo boom >&2; sleep 60"],
        what="never ready",
        log_path=tmp_path / "svc.log",
        ready=lambda: False,
        ready_timeout=1.0,
    )
    with pytest.raises(RuntimeError, match="never ready"):
        svc.start()
    # process must not be left running
    assert svc._proc is None


def test_container_run_args(tmp_path):
    svc = ContainerService(
        "podman",
        name="sw-kr-x-1",
        image="img:1",
        what="x",
        ready=lambda: True,
        cmd=["-c", "/data/kms.toml"],
        env={"A": "1"},
        mounts=[(tmp_path, "/data")],
        ports={15000: 5696},
        user="0",
    )
    args = svc.run_args()
    assert args[:4] == ["run", "-d", "--name", "sw-kr-x-1"]
    assert args[4:6] == ["-p", "127.0.0.1:15000:5696"]
    assert ["-v", f"{tmp_path}:/data:z"] == args[6:8]
    assert args[8:10] == ["-e", "A=1"]
    assert args[10:12] == ["--user", "0"]
    assert args[12:] == ["img:1", "-c", "/data/kms.toml"]


def _readable_svc():
    return ContainerService(
        "podman",
        name="sw-kr-x-1",
        image="img:1",
        what="x",
        ready=lambda: True,
        world_readable=["/out", "/data"],
    )


def test_make_readable_chmods_each_path(monkeypatch):
    calls = []

    def fake_run(argv, **kw):
        calls.append(argv)
        return subprocess.CompletedProcess(argv, 0, "", "")

    monkeypatch.setattr(provision.subprocess, "run", fake_run)
    _readable_svc()._make_readable()
    assert calls == [
        ["podman", "exec", "-u", "0", "sw-kr-x-1", "chmod", "-R", "a+r", "/out"],
        ["podman", "exec", "-u", "0", "sw-kr-x-1", "chmod", "-R", "a+r", "/data"],
    ]


def test_make_readable_raises_on_failure(monkeypatch):
    def fake_run(argv, **kw):
        return subprocess.CompletedProcess(argv, 1, "", "boom")

    monkeypatch.setattr(provision.subprocess, "run", fake_run)
    with pytest.raises(RuntimeError, match="chmod /out failed: boom"):
        _readable_svc()._make_readable()


def test_detect_runtime_none(monkeypatch):
    detect_runtime.cache_clear()
    monkeypatch.setattr(provision.shutil, "which", lambda _: None)
    assert detect_runtime() is None
    assert detect_runtime("docker") is None
    detect_runtime.cache_clear()


def test_check_reachable(tmp_path):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        s.listen(1)
        port = s.getsockname()[1]
        check_reachable(f"https://127.0.0.1:{port}")
    with pytest.raises(RuntimeError, match="not reachable"):
        check_reachable(f"https://127.0.0.1:{port}")
    with pytest.raises(RuntimeError, match="cannot parse"):
        check_reachable("nonsense")

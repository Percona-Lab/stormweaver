import pytest
from stormweaver.wrappers import (
    ExecPrefixWrapper,
    RRWrapper,
    ServerWrapper,
    ValgrindWrapper,
    WrapCtx,
    describe_exit,
    make_wrapper,
)


def ctx(tmp_path, session=1, run_dir=True):
    return WrapCtx(
        node="primary",
        datadir=tmp_path / "d",
        run_dir=(tmp_path / "run") if run_dir else None,
        session=session,
    )


def test_base_wrapper_is_passthrough(tmp_path):
    w = ServerWrapper()
    c = ctx(tmp_path)
    assert w.wrap_command(["postgres", "-D", "x"], c) == ["postgres", "-D", "x"]
    assert w.env(c) == {}
    assert w.time_multiplier == 1.0


def test_rr_wrap_command(tmp_path):
    w = RRWrapper(extra_args=["--chaos"])
    cmd = w.wrap_command(["postgres", "-D", "x"], ctx(tmp_path, session=3))
    assert cmd[:3] == ["rr", "record", "-o"]
    assert cmd[3] == str(tmp_path / "run" / "rr" / "primary-s03")
    assert cmd[4:] == ["--chaos", "postgres", "-D", "x"]
    assert (tmp_path / "run" / "rr").is_dir()
    assert not (tmp_path / "run" / "rr" / "primary-s03").exists()


def test_rr_deletes_clean_trace(tmp_path):
    w = RRWrapper()
    c = ctx(tmp_path)
    trace = w.trace_dir(c)
    trace.mkdir(parents=True)
    w.on_session_end(c, crashed=False)
    assert not trace.exists()


def test_rr_keeps_crashed_trace(tmp_path):
    w = RRWrapper()
    c = ctx(tmp_path)
    trace = w.trace_dir(c)
    trace.mkdir(parents=True)
    w.on_session_end(c, crashed=True)
    assert trace.exists()


def test_rr_keep_all_keeps_clean_trace(tmp_path):
    w = RRWrapper(keep_all=True)
    c = ctx(tmp_path)
    trace = w.trace_dir(c)
    trace.mkdir(parents=True)
    w.on_session_end(c, crashed=False)
    assert trace.exists()


def test_valgrind_wrap_command(tmp_path):
    w = ValgrindWrapper(extra_args=["--leak-check=full"])
    cmd = w.wrap_command(["mysqld"], ctx(tmp_path, session=2))
    assert cmd[0] == "valgrind"
    log_arg = cmd[1]
    assert log_arg.startswith("--log-file=")
    assert "valgrind-primary-s02" in log_arg
    assert "%p" in log_arg
    assert cmd[2:] == ["--leak-check=full", "mysqld"]
    assert w.time_multiplier == 20.0


def test_exec_prefix_wrapper(tmp_path):
    w = ExecPrefixWrapper(["strace", "-f"], time_multiplier=3.0)
    assert w.wrap_command(["postgres"], ctx(tmp_path)) == ["strace", "-f", "postgres"]
    assert w.time_multiplier == 3.0


def test_wrapctx_out_dir_falls_back_to_datadir(tmp_path):
    c = ctx(tmp_path, run_dir=False)
    assert c.out_dir() == tmp_path / "d"


def test_describe_exit():
    assert describe_exit(-9) == "SIGKILL"
    assert describe_exit(-11) == "SIGSEGV"
    assert describe_exit(0) == "0"
    assert describe_exit(1) == "1"
    assert describe_exit(None) == "unknown"


def test_make_wrapper_none_when_nothing_requested():
    assert make_wrapper(None, None, [], False) is None


def test_make_wrapper_cmd_builds_exec_prefix():
    w = make_wrapper(None, "env -i", [], False)
    assert isinstance(w, ExecPrefixWrapper)
    assert w.argv == ["env", "-i"]


def test_make_wrapper_rejects_name_and_cmd():
    with pytest.raises(RuntimeError, match="mutually exclusive"):
        make_wrapper("rr", "env", [], False)


def test_make_wrapper_rejects_unknown_name():
    with pytest.raises(RuntimeError, match="unknown wrapper"):
        make_wrapper("gdb", None, [], False)


def test_make_wrapper_rr_runs_preflight(monkeypatch):
    ran = []
    monkeypatch.setattr(RRWrapper, "preflight", lambda self: ran.append(1))
    w = make_wrapper("rr", None, ["--chaos"], True)
    assert isinstance(w, RRWrapper)
    assert w.extra_args == ["--chaos"]
    assert w.keep_all is True
    assert ran == [1]


def test_exec_prefix_preflight_rejects_missing_tool():
    w = ExecPrefixWrapper(["definitely-not-a-real-tool-xyz"])
    with pytest.raises(RuntimeError, match="not found"):
        w.preflight()


def test_spawn_failure_raises_with_command(tmp_path):
    import subprocess

    w = ExecPrefixWrapper(["definitely-not-a-real-tool-xyz"])
    with pytest.raises(RuntimeError, match="failed to spawn"):
        w.spawn(["postgres"], ctx(tmp_path), subprocess.DEVNULL, subprocess.DEVNULL)


def test_make_wrapper_rejects_cmd_with_extra_args():
    with pytest.raises(RuntimeError, match="wrapper-arg"):
        make_wrapper(None, "strace", ["-f"], False)

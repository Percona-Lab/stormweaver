import argparse
import time

import pytest
from stormweaver import scenario


def make_opts(tmp_path, extra=None, install_dir="", pgroot="", extend=None):
    """Emulate the cli: build a parser, add scenario opts, parse, finalize."""
    cfg = tmp_path / "stormweaver.toml"
    cfg.write_text(f'[default]\npgroot = "{pgroot}"\n')
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=str(cfg))
    parser.add_argument("--install-dir", default=install_dir)
    scenario.add_common_arguments(parser)
    if extend:
        extend(parser)
    args = parser.parse_args(extra or [])
    return scenario.finalize(args)


def test_parse_defaults(tmp_path):
    opts = make_opts(tmp_path, install_dir="/opt/pg")
    assert opts.duration == 10
    assert opts.workers == 5
    assert opts.repeat == 5
    assert opts.tde == "off"
    assert opts.pgsm == "off"
    assert opts.install_dir == "/opt/pg"


def test_parse_options(tmp_path):
    opts = make_opts(
        tmp_path,
        extra=["--duration", "3", "--workers", "2", "--repeat", "7", "--tde", "on"],
        install_dir="/opt/pg",
    )
    assert (opts.duration, opts.workers, opts.repeat, opts.tde) == (3, 2, 7, "on")


def test_parse_install_dir_from_config(tmp_path):
    opts = make_opts(tmp_path, pgroot="/from/config")
    assert opts.install_dir == "/from/config"


def test_parse_requires_install_dir(tmp_path):
    with pytest.raises(RuntimeError, match="install dir"):
        make_opts(tmp_path)


def test_parse_extend(tmp_path):
    def extend(parser):
        parser.add_argument("--extra-flag", type=int, default=42)
        parser.set_defaults(duration=30)

    opts = make_opts(tmp_path, install_dir="/opt/pg", extend=extend)
    assert opts.extra_flag == 42
    assert opts.duration == 30


def test_fresh_dir(tmp_path):
    a = tmp_path / "a"
    a.mkdir()
    (a / "junk").write_text("x")
    b = tmp_path / "b"
    scenario.fresh_dir(a, b)
    assert a.is_dir() and not list(a.iterdir())
    assert b.is_dir()


def test_fresh_dir_replaces_file(tmp_path):
    a = tmp_path / "a"
    a.write_text("not a dir")
    scenario.fresh_dir(a)
    assert a.is_dir() and not list(a.iterdir())


def test_clear_old_logs_keeps_active_dir(tmp_path, monkeypatch):
    base = tmp_path / "logs"
    base.mkdir()
    old1 = base / "run1"
    old2 = base / "run2"
    active = base / "run3"
    old1.mkdir()
    old2.mkdir()
    active.mkdir()
    (old1 / "server.log").write_text("x")
    monkeypatch.setattr(scenario.swlog, "log_dir", lambda: active)

    scenario._clear_old_logs(base_dir=base)

    assert not old1.exists()
    assert not old2.exists()
    assert active.is_dir()


def test_clear_old_logs_missing_base_is_noop(tmp_path, monkeypatch):
    base = tmp_path / "does-not-exist"
    monkeypatch.setattr(scenario.swlog, "log_dir", lambda: None)

    scenario._clear_old_logs(base_dir=base)


def test_parse_clear_logs_triggers_cleanup(tmp_path, monkeypatch):
    called = {}
    monkeypatch.setattr(
        scenario, "_clear_old_logs", lambda: called.setdefault("hit", True)
    )
    make_opts(tmp_path, extra=["--clear-logs"], install_dir="/opt/pg")
    assert called.get("hit")


def test_wait_for_log_found(tmp_path):
    log = tmp_path / "server.log"
    log.write_text("before\npausing at the end of recovery\n")
    assert scenario.wait_for_log(log, "pausing at the end of recovery", timeout=1)


def test_wait_for_log_respects_offset(tmp_path):
    log = tmp_path / "server.log"
    log.write_text("pausing at the end of recovery\n")
    offset = log.stat().st_size
    assert not scenario.wait_for_log(
        log, "pausing at the end of recovery", timeout=0.5, offset=offset
    )


def test_wait_for_log_timeout(tmp_path):
    log = tmp_path / "server.log"
    log.write_text("nothing\n")
    start = time.monotonic()
    assert not scenario.wait_for_log(log, "absent", timeout=0.5)
    assert time.monotonic() - start < 5


def test_parse_no_wrapper_by_default(tmp_path):
    opts = make_opts(tmp_path, install_dir="/opt/pg")
    assert opts.wrapper is None


def test_parse_wrapper_cmd(tmp_path):
    from stormweaver.wrappers import ExecPrefixWrapper

    opts = make_opts(tmp_path, extra=["--wrapper-cmd", "env"], install_dir="/opt/pg")
    assert isinstance(opts.wrapper, ExecPrefixWrapper)
    assert opts.wrapper.argv == ["env"]


def test_parse_wrapper_and_cmd_conflict(tmp_path):
    with pytest.raises(SystemExit):
        make_opts(
            tmp_path,
            extra=["--wrapper", "rr", "--wrapper-cmd", "env"],
            install_dir="/opt/pg",
        )


def test_parse_wrapper_rr(tmp_path, monkeypatch):
    from stormweaver.wrappers import RRWrapper

    monkeypatch.setattr(RRWrapper, "preflight", lambda self: None)
    opts = make_opts(
        tmp_path,
        extra=["--wrapper", "rr", "--wrapper-arg=--chaos", "--keep-traces"],
        install_dir="/opt/pg",
    )
    assert isinstance(opts.wrapper, RRWrapper)
    assert opts.wrapper.extra_args == ["--chaos"]
    assert opts.wrapper.keep_all is True

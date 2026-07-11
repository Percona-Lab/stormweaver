import datetime
import logging
import re
from pathlib import Path

import pytest
import stormweaver._stormweaver as _stormweaver
from stormweaver import log as swlog

LINE_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2},\d{3} \[[A-Z]+\] [\w.-]+: .+$"
)


@pytest.fixture(autouse=True)
def _restore_log_state(tmp_path):
    saved = swlog._run_dir
    saved_mode = swlog._mode
    yield
    swlog._run_dir = saved
    swlog._mode = saved_mode
    _stormweaver.init_core_logging(str(tmp_path), swlog._forward, 2)


def test_init_logging_creates_dir_and_main_log(tmp_path):
    run_dir = swlog.init_logging(tmp_path / "run")
    logging.getLogger("fmt-test").info("hello")
    assert run_dir == tmp_path / "run"
    assert swlog.log_dir() == run_dir
    content = (run_dir / "main.log").read_text()
    assert re.search(
        r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2},\d{3} \[INFO\] fmt-test: hello$",
        content,
        re.M,
    )


def test_core_messages_reach_python(tmp_path, caplog):
    # no init_logging here: basicConfig(force=True) would remove pytest's
    # caplog handler from the root logger. Install just the forwarder.
    _stormweaver.init_core_logging(str(tmp_path), swlog._forward, 2)
    with caplog.at_level(logging.INFO):
        _stormweaver._core_log(2, "from core")
    rec = next(r for r in caplog.records if r.message == "from core")
    assert rec.name == "stormweaver.core"
    assert rec.levelno == logging.INFO


def test_core_and_python_lines_share_format(tmp_path):
    run_dir = swlog.init_logging(tmp_path / "run")
    logging.getLogger("pyside").info("py msg")
    _stormweaver._core_log(3, "core msg")
    lines = (run_dir / "main.log").read_text().splitlines()
    assert len(lines) == 2
    assert all(LINE_RE.match(line) for line in lines)
    assert "[WARNING] stormweaver.core: core msg" in lines[1]


def test_run_dir_naming_and_collision(tmp_path, monkeypatch):
    class FrozenDatetime:
        @staticmethod
        def now() -> datetime.datetime:
            return datetime.datetime(2026, 7, 6, 9, 44, 31)

    monkeypatch.setattr(swlog, "datetime", FrozenDatetime)
    d1 = swlog.init_run_logging("basic", base_dir=tmp_path)
    d2 = swlog.init_run_logging("basic", base_dir=tmp_path)
    assert d1 == tmp_path / "2026-07-06_09-44-31-basic"
    assert d2 == tmp_path / "2026-07-06_09-44-31-basic-2"


def test_run_dir_absolute_survives_chdir(tmp_path, monkeypatch):
    # relative base_dir like the real "logs": the run dir must be captured as an
    # absolute path so a later chdir (e.g. a scenario test) can't break server
    # log paths derived from log_dir()
    monkeypatch.chdir(tmp_path)
    run_dir = swlog.init_run_logging("chdirtest", base_dir="logs")
    assert run_dir.is_absolute()
    other = tmp_path / "elsewhere"
    other.mkdir()
    monkeypatch.chdir(other)
    assert swlog.log_dir() == run_dir
    # would raise FileNotFoundError if log_dir() were a stale relative path
    (swlog.log_dir() / "probe.log").write_text("ok")


def test_level_mapping():
    assert swlog._py_to_spdlog(logging.DEBUG) == 1
    assert swlog._py_to_spdlog(logging.INFO) == 2
    assert swlog._py_to_spdlog(logging.WARNING) == 3
    assert swlog._py_to_spdlog(logging.ERROR) == 4
    assert swlog._py_to_spdlog(logging.CRITICAL) == 5


def test_shutdown_registered_once(tmp_path, monkeypatch):
    registered = []
    monkeypatch.setattr(swlog.atexit, "register", registered.append)
    monkeypatch.setattr(swlog, "_shutdown_registered", False)
    swlog.init_logging(tmp_path / "a")
    swlog.init_logging(tmp_path / "b")
    assert registered == [_stormweaver.shutdown_core_logging]


def test_record_outcome_appends_lines(tmp_path):
    swlog.init_logging(tmp_path / "run")
    swlog.record_outcome("node=primary session=1 result=clean exit=0")
    swlog.record_outcome("scenario result=passed")
    text = (tmp_path / "run" / "outcome").read_text()
    assert text.splitlines() == [
        "node=primary session=1 result=clean exit=0",
        "scenario result=passed",
    ]


def test_record_outcome_noop_without_run_dir(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    monkeypatch.setattr(swlog, "_run_dir", None)
    swlog.record_outcome("scenario result=passed")
    assert list(tmp_path.iterdir()) == []


def test_record_outcome_swallows_os_errors(tmp_path, monkeypatch):
    monkeypatch.setattr(swlog, "_run_dir", tmp_path / "gone" / "deeper")
    swlog.record_outcome("scenario result=passed")


def test_unified_named_logger_reaches_main_log(tmp_path):
    run_dir = swlog.init_logging(tmp_path / "run", mode="unified")
    _stormweaver._file_log(
        "sql-conn-uni1", "sql-conn-uni1.log", 2, "Statement: SELECT 1"
    )
    assert swlog.log_mode() == "unified"
    content = (run_dir / "main.log").read_text()
    assert "[INFO] sql-conn-uni1: Statement: SELECT 1" in content
    assert not (run_dir / "sql-conn-uni1.log").exists()


def test_unified_splits_also_write_files(tmp_path):
    run_dir = swlog.init_logging(tmp_path / "run", mode="unified", splits=True)
    _stormweaver._file_log("sql-conn-uni2", "sql-conn-uni2.log", 2, "hello")
    assert "sql-conn-uni2: hello" in (run_dir / "main.log").read_text()
    assert "hello" in (run_dir / "sql-conn-uni2.log").read_text()


def test_quiet_unified_keeps_info_in_main_log(tmp_path):
    run_dir = swlog.init_logging(
        tmp_path / "run", level=logging.WARNING, mode="unified"
    )
    _stormweaver._file_log(
        "sql-conn-quiet1", "sql-conn-quiet1.log", 2, "Statement: SELECT 1"
    )
    logging.getLogger("test.quiet").info("ASSERT status=pass kind=x")
    content = (run_dir / "main.log").read_text()
    assert "sql-conn-quiet1: Statement: SELECT 1" in content
    assert "ASSERT status=pass" in content


def test_split_mode_keeps_named_loggers_out_of_main_log(tmp_path):
    run_dir = swlog.init_logging(tmp_path / "run")
    _stormweaver._file_log("sql-conn-split1", "sql-conn-split1.log", 2, "hello")
    assert "sql-conn-split1" not in (run_dir / "main.log").read_text()
    assert "hello" in (run_dir / "sql-conn-split1.log").read_text()


def test_init_logging_rejects_unknown_mode(tmp_path):
    with pytest.raises(ValueError):
        swlog.init_logging(tmp_path / "run", mode="both")


def test_console_filter_hides_connection_noise():
    def rec(name):
        return logging.LogRecord(name, logging.INFO, "", 0, "m", None, None)

    assert swlog._console_filter(rec("sql-conn-x")) is False
    assert swlog._console_filter(rec("worker-1-1")) is False
    assert swlog._console_filter(rec("test.node")) is True


def test_record_outcome_logs_event(tmp_path):
    run_dir = swlog.init_logging(tmp_path / "run")
    swlog.record_outcome("scenario result=passed")
    assert "OUTCOME scenario result=passed" in (run_dir / "main.log").read_text()


def test_ensure_logging_defaults_to_unified(monkeypatch):
    from stormweaver.testing import util

    calls = {}

    def fake_init(name, mode="split", splits=False):
        calls.update(name=name, mode=mode)
        return Path("x")

    monkeypatch.setattr(swlog, "_run_dir", None)
    monkeypatch.setattr(util.swlog, "init_run_logging", fake_init)
    util.ensure_logging()
    assert calls["mode"] == "unified"

import logging
from pathlib import Path

import pytest
from stormweaver import log as swlog
from stormweaver.cli import main, parse_args


@pytest.fixture
def _restore_log_state():
    saved_run_dir = swlog._run_dir
    saved_handlers = logging.root.handlers[:]
    yield
    swlog._run_dir = saved_run_dir
    for handler in logging.root.handlers[:]:
        if handler not in saved_handlers:
            handler.close()
    logging.root.handlers[:] = saved_handlers


def test_extra_args_collected():
    args = parse_args(["scen.py", "-i", "/pg", "--repeat", "7"])
    assert args.scenario == "scen.py"
    assert args.install_dir == "/pg"
    assert args.extra == ["--repeat", "7"]


def test_no_extra_args_is_empty_list():
    args = parse_args(["scen.py"])
    assert args.extra == []


def test_prefix_like_scenario_flag_not_swallowed():
    args = parse_args(["scen.py", "--conf", "x"])
    assert args.config == "config/stormweaver.toml"
    assert args.extra == ["--conf", "x"]


def test_cli_records_scenario_outcome(tmp_path, monkeypatch, _restore_log_state):
    monkeypatch.chdir(tmp_path)
    scen = tmp_path / "scen.py"
    scen.write_text("def main(args):\n    return 0\n")
    assert main([str(scen)]) == 0
    outcomes = list(Path("logs").glob("*/outcome"))
    assert len(outcomes) == 1
    assert "scenario result=passed" in outcomes[0].read_text()


def test_cli_records_failed_outcome(tmp_path, monkeypatch, _restore_log_state):
    monkeypatch.chdir(tmp_path)
    scen = tmp_path / "scen.py"
    scen.write_text("def main(args):\n    raise RuntimeError('boom')\n")
    assert main([str(scen)]) == 1
    outcomes = list(Path("logs").glob("*/outcome"))
    assert len(outcomes) == 1
    assert "scenario result=failed" in outcomes[0].read_text()


def test_cli_records_missing_main_as_failed(tmp_path, monkeypatch, _restore_log_state):
    monkeypatch.chdir(tmp_path)
    scen = tmp_path / "scen.py"
    scen.write_text("x = 1\n")
    assert main([str(scen)]) == 1
    outcomes = list(Path("logs").glob("*/outcome"))
    assert len(outcomes) == 1
    assert "scenario result=failed" in outcomes[0].read_text()


def _main_log(base=Path("logs")):
    return next(base.glob("*/main.log")).read_text()


def test_cli_expected_error_is_clean(tmp_path, monkeypatch, _restore_log_state):
    monkeypatch.chdir(tmp_path)
    scen = tmp_path / "scen.py"
    scen.write_text("def main(args):\n    raise RuntimeError('rr not found')\n")
    assert main([str(scen)]) == 1
    log = _main_log()
    assert "scenario failed: rr not found" in log
    assert "Traceback" not in log


def test_cli_expected_error_traceback_with_verbose(
    tmp_path, monkeypatch, _restore_log_state
):
    monkeypatch.chdir(tmp_path)
    scen = tmp_path / "scen.py"
    scen.write_text("def main(args):\n    raise RuntimeError('rr not found')\n")
    assert main(["-v", str(scen)]) == 1
    assert "Traceback" in _main_log()


def test_cli_unexpected_error_keeps_traceback(
    tmp_path, monkeypatch, _restore_log_state
):
    monkeypatch.chdir(tmp_path)
    scen = tmp_path / "scen.py"
    scen.write_text("def main(args):\n    raise KeyError('oops')\n")
    assert main([str(scen)]) == 1
    log = _main_log()
    assert "Traceback" in log
    outcomes = list(Path("logs").glob("*/outcome"))
    assert "scenario result=failed" in outcomes[0].read_text()


def test_cli_systemexit_message_logged_and_recorded(
    tmp_path, monkeypatch, _restore_log_state
):
    monkeypatch.chdir(tmp_path)
    scen = tmp_path / "scen.py"
    scen.write_text("def main(args):\n    raise SystemExit('need --tde on')\n")
    assert main([str(scen)]) == 1
    log = _main_log()
    assert "scenario failed: need --tde on" in log
    assert "Traceback" not in log
    outcomes = list(Path("logs").glob("*/outcome"))
    assert "scenario result=failed" in outcomes[0].read_text()


def test_cli_systemexit_code_preserved(tmp_path, monkeypatch, _restore_log_state):
    monkeypatch.chdir(tmp_path)
    scen = tmp_path / "scen.py"
    scen.write_text("def main(args):\n    raise SystemExit(3)\n")
    assert main([str(scen)]) == 3
    outcomes = list(Path("logs").glob("*/outcome"))
    assert "scenario result=failed" in outcomes[0].read_text()


def test_cli_systemexit_zero_is_passed(tmp_path, monkeypatch, _restore_log_state):
    monkeypatch.chdir(tmp_path)
    scen = tmp_path / "scen.py"
    scen.write_text("def main(args):\n    raise SystemExit(0)\n")
    assert main([str(scen)]) == 0
    outcomes = list(Path("logs").glob("*/outcome"))
    assert "scenario result=passed" in outcomes[0].read_text()

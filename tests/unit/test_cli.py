import logging
from pathlib import Path

import pytest
from stormweaver import cli
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


def test_cli_missing_main_no_run_dir(tmp_path, monkeypatch, capsys):
    monkeypatch.chdir(tmp_path)
    scen = tmp_path / "scen.py"
    scen.write_text("x = 1\n")
    assert main([str(scen)]) == 1
    assert "no main()" in capsys.readouterr().err
    assert not Path("logs").exists()


def test_cli_load_error_no_run_dir(tmp_path, monkeypatch, capsys):
    monkeypatch.chdir(tmp_path)
    scen = tmp_path / "scen.py"
    scen.write_text("this is not python\n")
    assert main([str(scen)]) == 1
    assert "failed to load scenario" in capsys.readouterr().err
    assert not Path("logs").exists()


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


def test_cli_systemexit_odd_payload(tmp_path, monkeypatch, _restore_log_state):
    monkeypatch.chdir(tmp_path)
    scen = tmp_path / "scen.py"
    scen.write_text("def main(args):\n    raise SystemExit(RuntimeError('weird'))\n")
    assert main([str(scen)]) == 1
    assert "scenario failed: weird" in _main_log()
    outcomes = list(Path("logs").glob("*/outcome"))
    assert "scenario result=failed" in outcomes[0].read_text()


def _scenario(tmp_path, header=""):
    f = tmp_path / "scen.py"
    f.write_text(header + "def main(args):\n    return 0\n")
    return f


@pytest.fixture
def init_calls(monkeypatch, tmp_path):
    monkeypatch.delenv("STORMWEAVER_LOG_MODE", raising=False)
    monkeypatch.delenv("STORMWEAVER_LOG_SPLITS", raising=False)
    calls = {}

    def fake_init(name, level, mode="split", splits=False):
        calls.update(name=name, mode=mode, splits=splits)
        return tmp_path

    monkeypatch.setattr(cli, "init_run_logging", fake_init)
    return calls


def test_default_mode_is_split(tmp_path, init_calls):
    assert cli.main([str(_scenario(tmp_path))]) == 0
    assert init_calls["mode"] == "split"
    assert init_calls["splits"] is False


def test_scenario_declares_unified(tmp_path, init_calls):
    scen = _scenario(tmp_path, 'LOG_MODE = "unified"\n')
    assert cli.main([str(scen)]) == 0
    assert init_calls["mode"] == "unified"


def test_env_overrides_scenario(tmp_path, init_calls, monkeypatch):
    monkeypatch.setenv("STORMWEAVER_LOG_MODE", "split")
    scen = _scenario(tmp_path, 'LOG_MODE = "unified"\n')
    assert cli.main([str(scen)]) == 0
    assert init_calls["mode"] == "split"


def test_flag_overrides_env(tmp_path, init_calls, monkeypatch):
    monkeypatch.setenv("STORMWEAVER_LOG_MODE", "split")
    assert cli.main([str(_scenario(tmp_path)), "--log-mode", "unified"]) == 0
    assert init_calls["mode"] == "unified"


def test_scenario_splits_attr(tmp_path, init_calls):
    scen = _scenario(tmp_path, 'LOG_MODE = "unified"\nLOG_SPLITS = True\n')
    assert cli.main([str(scen)]) == 0
    assert init_calls["splits"] is True


def test_splits_env(tmp_path, init_calls, monkeypatch):
    monkeypatch.setenv("STORMWEAVER_LOG_SPLITS", "1")
    assert cli.main([str(_scenario(tmp_path))]) == 0
    assert init_calls["splits"] is True


def test_splits_flag(tmp_path, init_calls):
    assert cli.main([str(_scenario(tmp_path)), "--log-splits"]) == 0
    assert init_calls["splits"] is True


def test_bad_scenario_mode_rejected(tmp_path, init_calls, capsys):
    scen = _scenario(tmp_path, 'LOG_MODE = "both"\n')
    assert cli.main([str(scen)]) == 1
    assert "log mode" in capsys.readouterr().err


def test_run_header_first_event(tmp_path, monkeypatch, _restore_log_state):
    monkeypatch.chdir(tmp_path)
    monkeypatch.delenv("STORMWEAVER_LOG_MODE", raising=False)
    assert main([str(_scenario(tmp_path))]) == 0
    first = _main_log().splitlines()[0]
    assert "RUN " in first
    assert "scenario=scen" in first
    assert "mode=split" in first

import logging

import pytest
from stormweaver import events


@pytest.mark.parametrize(
    "value",
    [
        "simple",
        "with space",
        'quote"inside',
        "back\\slash",
        "multi\nline",
        "carriage\rreturn",
        "ends with backslash\\",
        "",
        "ünïcode",
        "a=b",
    ],
)
def test_field_round_trip(value):
    line = events.format_event("ASSERT", {"status": "pass", "query": value})
    parsed = events.parse_event(line)
    assert parsed == ("ASSERT", {"status": "pass", "query": value})


def test_bare_values_stay_unquoted():
    line = events.format_event(
        "NODE", {"event": "start", "port": "5432", "datadir": "/tmp/x"}
    )
    assert line == "NODE event=start port=5432 datadir=/tmp/x"


def test_parse_rejects_non_events():
    assert events.parse_event("Statement: SELECT 1") is None
    assert events.parse_event("lowercase key=1") is None
    assert events.parse_event("ASSERT status=pass garbage") is None


def test_parse_line_splits_prefix():
    line = "2026-07-10 12:00:01,123 [ERROR] test.node1: ASSERT status=fail"
    _ts, level, name, message = events.parse_line(line)
    assert (level, name, message) == ("ERROR", "test.node1", "ASSERT status=fail")
    assert events.parse_line("garbage") is None


def test_emit_goes_through_named_logger(caplog):
    with caplog.at_level(logging.INFO):
        events.emit("ASSERT", logger="test.n1", status="pass", kind="safe_sql")
    rec = caplog.records[-1]
    assert rec.name == "test.n1"
    assert events.parse_event(rec.message) == (
        "ASSERT",
        {"status": "pass", "kind": "safe_sql"},
    )


def test_emit_dict_allows_reserved_field_names(caplog):
    with caplog.at_level(logging.INFO):
        events.emit("ASSERT", {"level": "high"}, logger="test.n1")
    rec = caplog.records[-1]
    assert events.parse_event(rec.message) == ("ASSERT", {"level": "high"})


def test_file_round_trip(tmp_path):
    from stormweaver import log as swlog

    value = 'a\rb\nc with "quote"'
    logger = logging.getLogger("test.file_round_trip")
    logger.setLevel(logging.INFO)
    handler = logging.FileHandler(tmp_path / "x.log")
    handler.setFormatter(logging.Formatter(swlog.FORMAT))
    logger.addHandler(handler)
    try:
        events.emit("ASSERT", {"query": value}, logger=logger)
    finally:
        handler.close()
        logger.removeHandler(handler)
    text = (tmp_path / "x.log").read_text()
    matches = []
    for line in text.splitlines():
        parsed = events.parse_line(line)
        if parsed is None:
            continue
        event = events.parse_event(parsed[3])
        if event is not None:
            matches.append(event)
    assert matches == [("ASSERT", {"query": value})]


def test_dump_emits_header_and_block(caplog):
    with caplog.at_level(logging.ERROR):
        events.dump("traceback", "line1\nline2", logger="test.n1")
    msgs = [r.message for r in caplog.records]
    assert events.parse_event(msgs[0]) == ("DUMP", {"what": "traceback", "lines": "2"})
    assert msgs[1:] == ["| line1", "| line2"]


def test_step_marks_begin_and_end(caplog):
    with caplog.at_level(logging.INFO), events.step("create tables"):
        pass
    parsed = [events.parse_event(r.message) for r in caplog.records]
    assert parsed[0] == ("STEP", {"phase": "begin", "name": "create tables"})
    assert parsed[-1] == (
        "STEP",
        {"phase": "end", "name": "create tables", "status": "ok"},
    )


def test_step_failure_status(caplog):
    with (
        caplog.at_level(logging.INFO),
        pytest.raises(ValueError),
        events.step("boom"),
    ):
        raise ValueError("x")
    parsed = [events.parse_event(r.message) for r in caplog.records]
    assert parsed[-1] == ("STEP", {"phase": "end", "name": "boom", "status": "fail"})


def test_run_header_includes_argv_and_cwd(caplog):
    with caplog.at_level(logging.INFO):
        events.emit_run_header(scenario="demo")
    event, fields = events.parse_event(caplog.records[-1].message)
    assert event == "RUN"
    assert fields["scenario"] == "demo"
    assert "argv" in fields and "cwd" in fields

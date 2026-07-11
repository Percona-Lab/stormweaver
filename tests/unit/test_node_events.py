import logging
from types import SimpleNamespace

import pytest
import stormweaver._stormweaver as _sw
from stormweaver import events

# importing TestConn by name makes pytest try to collect it
from stormweaver.testing import node


class StubResult:
    def __init__(self, rows):
        self._rows = rows

    def rows(self):
        return self._rows


class StubRaw:
    def __init__(self, rows=None, error=None, res=None):
        self._rows = rows or []
        self._error = error
        self._res = res

    def safe_execute(self, query, params):
        if self._error:
            raise self._error
        return StubResult(self._rows)

    def execute(self, query, params):
        return self._res


class StubRes:
    def __init__(self, success, error_message="", rows=None):
        self._success = success
        self.error_message = error_message
        self._rows = rows or []

    def success(self):
        return self._success

    def rows(self):
        return self._rows


def _events(caplog):
    return [
        (r.name, events.parse_event(r.message))
        for r in caplog.records
        if events.parse_event(r.message)
    ]


def test_safe_sql_pass_emits_assert(caplog):
    conn = node.TestConn(StubRaw(rows=[["1"], ["2"]]), name="c1")
    with caplog.at_level(logging.INFO):
        assert conn.safe_sql("SELECT a FROM t") == [["1"], ["2"]]
    name, (event, fields) = _events(caplog)[-1]
    assert name == "test.c1"
    assert event == "ASSERT"
    assert fields["status"] == "pass"
    assert fields["kind"] == "safe_sql"
    assert fields["rows"] == "2"


def test_safe_sql_failure_emits_assert_and_traceback(caplog):
    conn = node.TestConn(StubRaw(error=_sw.SqlError("relation missing")), name="c2")
    with caplog.at_level(logging.INFO), pytest.raises(_sw.SqlError):
        conn.safe_sql("SELECT a FROM t")
    parsed = _events(caplog)
    assert ("ASSERT", "fail") in [(e, f.get("status")) for _, (e, f) in parsed]
    assert any(e == "DUMP" and f["what"] == "traceback" for _, (e, f) in parsed)


def test_sql_value_shape_failure_dumps_result(caplog):
    conn = node.TestConn(StubRaw(rows=[["1"], ["2"]]), name="c3")
    with caplog.at_level(logging.INFO), pytest.raises(AssertionError):
        conn.sql_value("SELECT a FROM t")
    parsed = [pe for _, pe in _events(caplog)]
    fail = next(f for e, f in parsed if e == "ASSERT" and f["status"] == "fail")
    assert fail["expected"] == "1x1"
    assert fail["actual"] == "2x1"
    assert any(e == "DUMP" and f["what"] == "result" for e, f in parsed)


def test_expect_error_pass_and_fail(caplog):
    ok = node.TestConn(StubRaw(res=StubRes(False, "duplicate key value")), name="c4")
    with caplog.at_level(logging.INFO):
        ok.expect_error("duplicate key", "INSERT ...")
    _e, f = _events(caplog)[-1][1]
    assert (f["status"], f["kind"]) == ("pass", "expect_error")

    caplog.clear()
    bad = node.TestConn(StubRaw(res=StubRes(True, rows=[["1"]])), name="c5")
    with caplog.at_level(logging.INFO), pytest.raises(AssertionError):
        bad.expect_error("duplicate key", "INSERT ...")
    parsed = [pe for _, pe in _events(caplog)]
    assert any(e == "ASSERT" and f["status"] == "fail" for e, f in parsed)
    assert any(e == "DUMP" and f["what"] == "result" for e, f in parsed)


def test_failure_dumps_server_log_tail(caplog, tmp_path):
    server_log = tmp_path / "server.log"
    server_log.write_text("\n".join(f"line {i}" for i in range(60)) + "\n")
    fake_node = SimpleNamespace(name="n1", server_log=server_log)
    conn = node.TestConn(
        StubRaw(error=_sw.SqlError("relation missing")), name="c6", node=fake_node
    )
    with caplog.at_level(logging.INFO), pytest.raises(_sw.SqlError):
        conn.safe_sql("SELECT a FROM t")
    records = caplog.records
    idx = next(
        i
        for i, r in enumerate(records)
        if (pe := events.parse_event(r.message))
        and pe[0] == "DUMP"
        and pe[1]["what"] == "server-log-tail"
    )
    _, fields = events.parse_event(records[idx].message)
    assert fields["node"] == "n1"
    assert fields["lines"] == "50"
    tail = [r.message for r in records[idx + 1 : idx + 51]]
    assert len(tail) == 50
    assert all(m.startswith("| ") for m in tail)
    assert tail[0] == "| line 10"
    assert tail[-1] == "| line 59"


def test_missing_server_log_does_not_mask_failure(caplog, tmp_path):
    fake_node = SimpleNamespace(name="n1", server_log=tmp_path / "nope.log")
    conn = node.TestConn(
        StubRaw(error=_sw.SqlError("relation missing")), name="c7", node=fake_node
    )
    with caplog.at_level(logging.INFO), pytest.raises(_sw.SqlError):
        conn.safe_sql("SELECT a FROM t")
    parsed = [pe for _, pe in _events(caplog)]
    assert not any(e == "DUMP" and f["what"] == "server-log-tail" for e, f in parsed)
    assert any(r.levelno == logging.WARNING for r in caplog.records)

import csv
import logging
from dataclasses import dataclass, field

from stormweaver import stats_csv


@dataclass
class FakeTiming:
    _avg: float = 1.0
    _min: float = 0.5
    _max: float = 2.0
    _hist: list[int] = field(default_factory=lambda: [1, 2, 3, 4, 5, 6, 7])
    count: int = 28

    def avg_ms(self):
        return self._avg

    def min_ms(self):
        return self._min

    def max_ms(self):
        return self._max

    def histogram(self):
        return self._hist

    def has_data(self):
        return self.count > 0


@dataclass
class FakeAction:
    success_count: int = 10
    action_failure_count: int = 1
    sql_failure_count: int = 2
    other_failure_count: int = 0
    sql_conflict_count: int = 3
    action_error_names: dict = field(default_factory=dict)
    sql_error_codes: dict = field(default_factory=lambda: {"40001": 3})
    execution_timing: FakeTiming = field(default_factory=FakeTiming)
    sql_timing: FakeTiming = field(default_factory=FakeTiming)

    def success_rate(self):
        return 62.5

    def row_histograms(self):
        return {"select": [4, 1, 0, 0, 0, 0]}


@dataclass
class FakeTxn:
    committed: int = 5
    rolled_back_intentional: int = 2
    rolled_back_error: int = 1
    implicit_commits: int = 0
    savepoint_rollbacks: int = 4
    sub_actions_ok: int = 20
    sub_actions_fail: int = 3

    def has_data(self):
        return True

    def sub_histogram(self):
        return [0, 1, 7, 0, 0, 0]


class FakeStats:
    def action_names(self):
        return ["select_all"]

    def action_stats(self, name):
        assert name == "select_all"
        return FakeAction()

    def transaction_stats(self):
        return FakeTxn()


def _read(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def test_writes_all_files(tmp_path):
    stats_csv.append_stats(tmp_path, 1, [("w1", FakeStats())])

    stats = _read(tmp_path / "stats.csv")
    assert stats[0]["worker"] == "w1"
    assert stats[0]["cycle"] == "1"
    assert stats[0]["action"] == "select_all"
    assert stats[0]["success"] == "10"
    assert stats[0]["conflict"] == "3"

    hists = _read(tmp_path / "histograms.csv")
    assert hists[0]["stmt_kind"] == "select"
    assert hists[0]["rows_0"] == "4"

    timings = _read(tmp_path / "timings.csv")
    kinds = {r["timing_kind"] for r in timings}
    assert kinds == {"exec", "sql"}
    assert timings[0]["t_lt_0_1ms"] == "1"

    errors = _read(tmp_path / "errors.csv")
    assert errors[0]["error_code"] == "40001"
    assert errors[0]["count"] == "3"

    txns = _read(tmp_path / "transactions.csv")
    assert txns[0]["committed"] == "5"
    assert txns[0]["sub_2_10"] == "7"


def test_appends_without_duplicate_header(tmp_path):
    stats_csv.append_stats(tmp_path, 1, [("w1", FakeStats())])
    stats_csv.append_stats(tmp_path, 2, [("w1", FakeStats())])
    rows = _read(tmp_path / "stats.csv")
    assert len(rows) == 2
    assert {r["cycle"] for r in rows} == {"1", "2"}


class EmptyAction(FakeAction):
    def row_histograms(self):
        return {}


class EmptyTxn(FakeTxn):
    def has_data(self):
        return False

    def sub_histogram(self):
        raise AssertionError("must not be called without data")


class EmptyStats:
    def action_names(self):
        return ["noop"]

    def action_stats(self, name):
        return EmptyAction(
            success_count=0,
            action_failure_count=0,
            sql_failure_count=0,
            other_failure_count=0,
            sql_conflict_count=0,
            sql_error_codes={},
            execution_timing=FakeTiming(count=0),
            sql_timing=FakeTiming(count=0),
        )

    def transaction_stats(self):
        return EmptyTxn()


class NoneStats:
    def action_names(self):
        return ["gone"]

    def action_stats(self, name):
        return None

    def transaction_stats(self):
        return EmptyTxn()


def test_skips_empty_data(tmp_path):
    stats_csv.append_stats(tmp_path, 1, [("w1", EmptyStats())])
    assert list(tmp_path.iterdir()) == []


def test_none_action_stats_skipped(tmp_path):
    stats_csv.append_stats(tmp_path, 1, [("w1", NoneStats())])
    assert list(tmp_path.iterdir()) == []


def test_io_error_does_not_raise(tmp_path, caplog):
    target = tmp_path / "not_a_dir"
    target.write_text("file, not dir")
    with caplog.at_level(logging.WARNING, logger="stormweaver.stats_csv"):
        stats_csv.append_stats(target, 1, [("w1", FakeStats())])  # must not raise
    assert any(
        r.name == "stormweaver.stats_csv" and r.levelno == logging.WARNING
        for r in caplog.records
    )

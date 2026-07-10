"""Long-format CSV statistics output, one row per (worker, cycle, action).

Duck-typed on the binding objects so tests can use plain fakes.
"""

import csv
import logging
from collections.abc import Iterable
from pathlib import Path
from typing import Any, Protocol

logger = logging.getLogger(__name__)

ROW_BUCKETS = [
    "rows_0",
    "rows_1",
    "rows_2_10",
    "rows_11_100",
    "rows_101_1000",
    "rows_1000_plus",
]
TIME_BUCKETS = [
    "t_lt_0_1ms",
    "t_0_1_1ms",
    "t_1_10ms",
    "t_10_100ms",
    "t_100ms_1s",
    "t_1_10s",
    "t_gt_10s",
]
SUB_BUCKETS = [
    "sub_0",
    "sub_1",
    "sub_2_10",
    "sub_11_100",
    "sub_101_1000",
    "sub_1000_plus",
]

STATS_HEADER = [
    "worker",
    "cycle",
    "action",
    "success",
    "action_fail",
    "sql_fail",
    "other_fail",
    "conflict",
    "success_rate",
    "exec_avg_ms",
    "exec_min_ms",
    "exec_max_ms",
    "sql_avg_ms",
    "sql_min_ms",
    "sql_max_ms",
]
HIST_HEADER = ["worker", "cycle", "action", "stmt_kind", *ROW_BUCKETS]
TIMING_HEADER = ["worker", "cycle", "action", "timing_kind", *TIME_BUCKETS]
ERRORS_HEADER = ["worker", "cycle", "action", "error_code", "count"]
TXN_HEADER = [
    "worker",
    "cycle",
    "committed",
    "rollback_intentional",
    "rollback_error",
    "implicit_commits",
    "savepoint_rollbacks",
    "sub_ok",
    "sub_fail",
    *SUB_BUCKETS,
]


class StatsLike(Protocol):
    def action_names(self) -> Iterable[str]: ...
    def action_stats(self, name: str) -> Any: ...
    def transaction_stats(self) -> Any: ...


def _append(path: Path, header: list[str], rows: list[list[Any]]) -> None:
    if not rows:
        return
    write_header = not path.exists()
    with open(path, "a", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        if write_header:
            writer.writerow(header)
        writer.writerows(rows)


def append_stats(
    log_dir: str | Path, cycle: int, named_stats: Iterable[tuple[str, StatsLike]]
) -> None:
    """Append one cycle's statistics. named_stats: [(worker_name, stats)]."""
    try:
        _append_stats(Path(log_dir), cycle, named_stats)
    except OSError:
        logger.warning("could not write statistics CSVs", exc_info=True)


def _append_stats(
    log_dir: Path, cycle: int, named_stats: Iterable[tuple[str, StatsLike]]
) -> None:
    stat_rows: list[list[Any]] = []
    hist_rows: list[list[Any]] = []
    timing_rows: list[list[Any]] = []
    error_rows: list[list[Any]] = []
    txn_rows: list[list[Any]] = []

    for name, stats in named_stats:
        for action in stats.action_names():
            act = stats.action_stats(action)
            if act is None:
                continue
            total = (
                act.success_count
                + act.action_failure_count
                + act.sql_failure_count
                + act.other_failure_count
                + act.sql_conflict_count
            )
            if total > 0:
                stat_rows.append(
                    [
                        name,
                        cycle,
                        action,
                        act.success_count,
                        act.action_failure_count,
                        act.sql_failure_count,
                        act.other_failure_count,
                        act.sql_conflict_count,
                        f"{act.success_rate():.2f}",
                        f"{act.execution_timing.avg_ms():.3f}",
                        f"{act.execution_timing.min_ms():.3f}",
                        f"{act.execution_timing.max_ms():.3f}",
                        f"{act.sql_timing.avg_ms():.3f}",
                        f"{act.sql_timing.min_ms():.3f}",
                        f"{act.sql_timing.max_ms():.3f}",
                    ]
                )
                for kind, timing in (
                    ("exec", act.execution_timing),
                    ("sql", act.sql_timing),
                ):
                    if timing.has_data():
                        timing_rows.append(
                            [name, cycle, action, kind, *timing.histogram()]
                        )
            for kind, buckets in sorted(act.row_histograms().items()):
                hist_rows.append([name, cycle, action, kind, *buckets])
            for code, count in sorted(act.sql_error_codes.items()):
                error_rows.append([name, cycle, action, code, count])
            for err, count in sorted(act.action_error_names.items()):
                error_rows.append([name, cycle, action, err, count])

        txn = stats.transaction_stats()
        if txn.has_data():
            txn_rows.append(
                [
                    name,
                    cycle,
                    txn.committed,
                    txn.rolled_back_intentional,
                    txn.rolled_back_error,
                    txn.implicit_commits,
                    txn.savepoint_rollbacks,
                    txn.sub_actions_ok,
                    txn.sub_actions_fail,
                    *txn.sub_histogram(),
                ]
            )

    _append(log_dir / "stats.csv", STATS_HEADER, stat_rows)
    _append(log_dir / "histograms.csv", HIST_HEADER, hist_rows)
    _append(log_dir / "timings.csv", TIMING_HEADER, timing_rows)
    _append(log_dir / "errors.csv", ERRORS_HEADER, error_rows)
    _append(log_dir / "transactions.csv", TXN_HEADER, txn_rows)

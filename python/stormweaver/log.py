import atexit
import logging
from datetime import datetime
from pathlib import Path

import stormweaver._stormweaver as _stormweaver

# single source of truth, C++ file loggers replicate this shape
FORMAT = "%(asctime)s [%(levelname)s] %(name)s: %(message)s"

logger = logging.getLogger(__name__)

_run_dir: Path | None = None
_shutdown_registered = False

# spdlog: trace=0 debug=1 info=2 warn=3 err=4 critical=5 off=6
_SPDLOG_TO_PY = {
    0: logging.DEBUG,
    1: logging.DEBUG,
    2: logging.INFO,
    3: logging.WARNING,
    4: logging.ERROR,
    5: logging.CRITICAL,
}

_mode = "split"


def _forward(level: int, name: str, msg: str) -> None:
    # called with the C++ sink's non-recursive mutex held:
    # never log back into spdlog from here, python logging only
    py_name = "stormweaver.core" if name == "core" else name
    logging.getLogger(py_name).log(_SPDLOG_TO_PY.get(level, logging.INFO), "%s", msg)


def _console_filter(record: logging.LogRecord) -> bool:
    # per-statement detail stays in main.log, keep the terminal readable
    return not record.name.startswith(("sql-conn-", "worker-"))


def _py_to_spdlog(level: int) -> int:
    if level <= logging.DEBUG:
        return 1
    if level <= logging.INFO:
        return 2
    if level <= logging.WARNING:
        return 3
    if level <= logging.ERROR:
        return 4
    return 5


def init_logging(
    run_dir: str | Path,
    level: int = logging.INFO,
    mode: str = "split",
    splits: bool = False,
) -> Path:
    global _run_dir, _shutdown_registered, _mode
    if mode not in ("split", "unified"):
        raise ValueError(f"unknown log mode: {mode}")
    # absolute at init time: server log paths derive from this global, and a
    # later chdir (some scenario tests do) must not strand a relative path
    run_dir = Path(run_dir).absolute()
    run_dir.mkdir(parents=True, exist_ok=True)
    # quiet must not thin the unified file: root stays at INFO+, console gates
    root_level = min(level, logging.INFO) if mode == "unified" else level
    console = logging.StreamHandler()
    console.setLevel(level)
    if mode == "unified" and level > logging.DEBUG:
        console.addFilter(_console_filter)
    logging.basicConfig(
        level=root_level,
        format=FORMAT,
        handlers=[console, logging.FileHandler(run_dir / "main.log")],
        force=True,
    )
    # mode is fixed once C++ loggers exist: re-init in a different mode does
    # not re-route already-registered loggers
    _stormweaver.init_core_logging(
        str(run_dir), _forward, _py_to_spdlog(root_level), mode == "unified", splits
    )
    if not _shutdown_registered:
        # swapping the sink while a C++ worker logs is use-after-free, but
        # Workload.run() always joins its workers, so none run at exit time
        atexit.register(_stormweaver.shutdown_core_logging)
        _shutdown_registered = True
    _run_dir = run_dir
    _mode = mode
    return run_dir


def init_run_logging(
    scenario_name: str,
    level: int = logging.INFO,
    base_dir: str | Path = "logs",
    mode: str = "split",
    splits: bool = False,
) -> Path:
    stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    base = Path(base_dir)
    candidate = base / f"{stamp}-{scenario_name}"
    n = 2
    while candidate.exists():
        candidate = base / f"{stamp}-{scenario_name}-{n}"
        n += 1
    return init_logging(candidate, level, mode=mode, splits=splits)


def log_dir() -> Path | None:
    return _run_dir


def log_mode() -> str:
    return _mode


def record_outcome(line: str) -> None:
    if _run_dir is None:
        return
    logger.info("OUTCOME %s", line)
    try:
        with open(_run_dir / "outcome", "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        logger.warning("could not record outcome", exc_info=True)

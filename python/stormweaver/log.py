import atexit
import logging
from datetime import datetime
from pathlib import Path

import stormweaver._stormweaver as _stormweaver

# single source of truth, C++ file loggers replicate this shape
FORMAT = "%(asctime)s [%(levelname)s] %(name)s: %(message)s"

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

_core_logger = logging.getLogger("stormweaver.core")


def _forward(level: int, msg: str) -> None:
    # called with the C++ callback sink's non-recursive mutex held:
    # never log back into spdlog from here, python logging only
    _core_logger.log(_SPDLOG_TO_PY.get(level, logging.INFO), "%s", msg)


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


def init_logging(run_dir: str | Path, level: int = logging.INFO) -> Path:
    global _run_dir, _shutdown_registered
    run_dir = Path(run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(
        level=level,
        format=FORMAT,
        handlers=[
            logging.StreamHandler(),
            logging.FileHandler(run_dir / "main.log"),
        ],
        force=True,
    )
    _stormweaver.init_core_logging(str(run_dir), _forward, _py_to_spdlog(level))
    if not _shutdown_registered:
        # swapping the sink while a C++ worker logs is use-after-free, but
        # Workload.run() always joins its workers, so none run at exit time
        atexit.register(_stormweaver.shutdown_core_logging)
        _shutdown_registered = True
    _run_dir = run_dir
    return run_dir


def init_run_logging(
    scenario_name: str, level: int = logging.INFO, base_dir: str | Path = "logs"
) -> Path:
    stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    base = Path(base_dir)
    candidate = base / f"{stamp}-{scenario_name}"
    n = 2
    while candidate.exists():
        candidate = base / f"{stamp}-{scenario_name}-{n}"
        n += 1
    return init_logging(candidate, level)


def log_dir() -> Path | None:
    return _run_dir

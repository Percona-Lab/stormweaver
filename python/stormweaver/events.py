import logging
import os
import re
import sys
from collections.abc import Iterator
from contextlib import contextmanager

_default_logger = logging.getLogger("scenario")

_BARE_RE = re.compile(r"^[A-Za-z0-9_./:-]+$")
_KIND_RE = re.compile(r"([A-Z][A-Z_]*)(?: |$)")
_FIELD_RE = re.compile(r'(\w+)=(?:"((?:[^"\\]|\\.)*)"|([A-Za-z0-9_./:-]+))(?: |$)')


def _quote(value: str) -> str:
    if value and _BARE_RE.match(value):
        return value
    escaped = (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
    )
    return f'"{escaped}"'


_UNESCAPE_MAP = {"n": "\n", "r": "\r"}


def _unescape(value: str) -> str:
    return re.sub(r"\\(.)", lambda m: _UNESCAPE_MAP.get(m.group(1), m.group(1)), value)


def format_event(event: str, fields: dict[str, str]) -> str:
    parts = [event]
    parts += [f"{key}={_quote(value)}" for key, value in fields.items()]
    return " ".join(parts)


def _resolve(logger: logging.Logger | str | None) -> logging.Logger:
    if isinstance(logger, str):
        return logging.getLogger(logger)
    return logger or _default_logger


def emit(
    event: str,
    fields: dict[str, str] | None = None,
    *,
    level: int = logging.INFO,
    logger: logging.Logger | str | None = None,
    **kwargs: str,
) -> None:
    merged = {**(fields or {}), **kwargs}
    _resolve(logger).log(level, "%s", format_event(event, merged))


def parse_event(message: str) -> tuple[str, dict[str, str]] | None:
    m = _KIND_RE.match(message)
    if not m:
        return None
    fields: dict[str, str] = {}
    pos = m.end()
    while pos < len(message):
        fm = _FIELD_RE.match(message, pos)
        if not fm:
            return None
        key, quoted, bare = fm.group(1), fm.group(2), fm.group(3)
        fields[key] = _unescape(quoted) if quoted is not None else bare
        pos = fm.end()
    return m.group(1), fields


_LINE_RE = re.compile(
    r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2},\d{3}) \[([A-Z]+)\] ([\w.-]+): (.*)$"
)


def parse_line(line: str) -> tuple[str, str, str, str] | None:
    m = _LINE_RE.match(line)
    if m is None:
        return None
    return (m.group(1), m.group(2), m.group(3), m.group(4))


def dump(
    what: str,
    text: str,
    *,
    level: int = logging.ERROR,
    logger: logging.Logger | str | None = None,
    **fields: str,
) -> None:
    lines = text.splitlines() or [""]
    emit(
        "DUMP",
        {"what": what, "lines": str(len(lines)), **fields},
        level=level,
        logger=logger,
    )
    lg = _resolve(logger)
    for line in lines:
        lg.log(level, "| %s", line)


@contextmanager
def step(name: str, *, logger: logging.Logger | str | None = None) -> Iterator[None]:
    emit("STEP", logger=logger, phase="begin", name=name)
    try:
        yield
    except BaseException:
        emit(
            "STEP",
            level=logging.ERROR,
            logger=logger,
            phase="end",
            name=name,
            status="fail",
        )
        raise
    emit("STEP", logger=logger, phase="end", name=name, status="ok")


def emit_run_header(**fields: str) -> None:
    env = " ".join(
        f"{k}={v}"
        for k, v in sorted(os.environ.items())
        if k.startswith("STORMWEAVER_")
    )
    run_fields = {**fields, "argv": " ".join(sys.argv), "cwd": os.getcwd()}
    if env:
        run_fields["env"] = env
    emit("RUN", run_fields)

import logging
import os
import shlex
import shutil
import signal
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import IO, Any

logger = logging.getLogger(__name__)


def describe_exit(rc: int | None) -> str:
    if rc is None:
        return "unknown"
    if rc < 0:
        try:
            return signal.Signals(-rc).name
        except ValueError:
            return f"signal{-rc}"
    return str(rc)


@dataclass
class WrapCtx:
    node: str
    datadir: Path
    run_dir: Path | None
    session: int

    def out_dir(self) -> Path:
        return self.run_dir if self.run_dir is not None else self.datadir


class ServerWrapper:
    time_multiplier: float = 1.0

    def preflight(self) -> None:
        pass

    def wrap_command(self, cmd: list[str], ctx: WrapCtx) -> list[str]:
        return cmd

    def env(self, ctx: WrapCtx) -> dict[str, str]:
        return {}

    def spawn(
        self, cmd: list[str], ctx: WrapCtx, stdout: IO[Any] | int, stderr: IO[Any] | int
    ) -> subprocess.Popen[bytes]:
        full_cmd = self.wrap_command(cmd, ctx)
        try:
            return subprocess.Popen(
                full_cmd,
                stdout=stdout,
                stderr=stderr,
                env=os.environ | self.env(ctx),
            )
        except OSError as e:
            raise RuntimeError(f"failed to spawn {shlex.join(full_cmd)}: {e}") from e

    def on_session_end(self, ctx: WrapCtx, crashed: bool) -> None:
        pass


class RRWrapper(ServerWrapper):
    time_multiplier = 2.0

    def __init__(
        self, extra_args: list[str] | None = None, keep_all: bool = False
    ) -> None:
        self.extra_args = extra_args or []
        self.keep_all = keep_all

    def trace_dir(self, ctx: WrapCtx) -> Path:
        return ctx.out_dir() / "rr" / f"{ctx.node}-s{ctx.session:02d}"

    def preflight(self) -> None:
        if shutil.which("rr") is None:
            raise RuntimeError("rr not found in PATH")
        # perf counter access is the usual environment failure, catch it early
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                ["rr", "record", "true"],
                capture_output=True,
                text=True,
                env=os.environ | {"_RR_TRACE_DIR": tmp},
            )
        if result.returncode != 0:
            raise RuntimeError(f"rr cannot record on this system: {result.stderr}")

    def wrap_command(self, cmd: list[str], ctx: WrapCtx) -> list[str]:
        trace = self.trace_dir(ctx)
        # rr insists on creating the trace dir itself, only make the parent
        trace.parent.mkdir(parents=True, exist_ok=True)
        return ["rr", "record", "-o", str(trace), *self.extra_args, *cmd]

    def on_session_end(self, ctx: WrapCtx, crashed: bool) -> None:
        trace = self.trace_dir(ctx)
        if crashed or self.keep_all:
            logger.info("keeping rr trace %s", trace)
            return
        shutil.rmtree(trace, ignore_errors=True)


class ValgrindWrapper(ServerWrapper):
    time_multiplier = 20.0

    def __init__(self, extra_args: list[str] | None = None) -> None:
        self.extra_args = extra_args or []

    def preflight(self) -> None:
        if shutil.which("valgrind") is None:
            raise RuntimeError("valgrind not found in PATH")

    def wrap_command(self, cmd: list[str], ctx: WrapCtx) -> list[str]:
        # %p: postgres forks backends, give each process its own log
        log = ctx.out_dir() / f"valgrind-{ctx.node}-s{ctx.session:02d}-%p.log"
        return ["valgrind", f"--log-file={log}", *self.extra_args, *cmd]


class ExecPrefixWrapper(ServerWrapper):
    def __init__(self, argv: list[str], time_multiplier: float = 1.0) -> None:
        self.argv = argv
        self.time_multiplier = time_multiplier

    def preflight(self) -> None:
        if not self.argv:
            raise RuntimeError("empty wrapper command")
        if shutil.which(self.argv[0]) is None:
            raise RuntimeError(f"wrapper command not found: {self.argv[0]}")

    def wrap_command(self, cmd: list[str], ctx: WrapCtx) -> list[str]:
        return [*self.argv, *cmd]


def make_wrapper(
    name: str | None,
    cmd: str | None,
    extra_args: list[str],
    keep_all: bool,
) -> ServerWrapper | None:
    if name and cmd:
        raise RuntimeError("--wrapper and --wrapper-cmd are mutually exclusive")
    if cmd and extra_args:
        raise RuntimeError(
            "--wrapper-arg requires --wrapper, put args in --wrapper-cmd instead"
        )
    wrapper: ServerWrapper | None = None
    if name == "rr":
        wrapper = RRWrapper(extra_args, keep_all)
    elif name == "valgrind":
        wrapper = ValgrindWrapper(extra_args)
    elif name:
        raise RuntimeError(f"unknown wrapper: {name}")
    elif cmd:
        wrapper = ExecPrefixWrapper(shlex.split(cmd))
    if wrapper is not None:
        wrapper.preflight()
    return wrapper

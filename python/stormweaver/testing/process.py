import logging
import os
import re
import shlex
import subprocess
from pathlib import Path

logger = logging.getLogger(__name__)


def run_bin(
    install_dir: str | Path,
    name: str,
    *args: str | Path,
    env: dict[str, str] | None = None,
    cwd: str | Path | None = None,
    input: str | None = None,
    timeout: float | None = None,
) -> subprocess.CompletedProcess[str]:
    exe = Path(install_dir) / "bin" / name
    if not exe.exists():
        raise RuntimeError(f"binary not found: {exe}")
    argv = [str(exe), *(str(a) for a in args)]
    logger.info("running: %s", shlex.join(argv))
    cp = subprocess.run(
        argv,
        capture_output=True,
        text=True,
        env=os.environ | (env or {}),
        cwd=cwd,
        input=input,
        # without input, keep tools from hanging on the parent's terminal
        stdin=subprocess.DEVNULL if input is None else None,
        timeout=timeout,
    )
    if cp.returncode != 0:
        logger.info("rc=%d stderr: %s", cp.returncode, cp.stderr.strip())
    return cp


def _describe(cp: subprocess.CompletedProcess[str]) -> str:
    args = cp.args if isinstance(cp.args, list) else [str(cp.args)]
    return shlex.join(str(a) for a in args)


def command_ok(cp: subprocess.CompletedProcess[str], msg: str = "") -> None:
    if cp.returncode != 0:
        raise AssertionError(
            f"{msg or _describe(cp)} failed rc={cp.returncode}\n"
            f"stdout: {cp.stdout}\nstderr: {cp.stderr}"
        )


def command_fails(
    cp: subprocess.CompletedProcess[str],
    stderr_re: str | None = None,
    msg: str = "",
) -> None:
    what = msg or _describe(cp)
    if cp.returncode == 0:
        raise AssertionError(f"{what} succeeded, expected failure\nstdout: {cp.stdout}")
    if stderr_re is not None and not re.search(stderr_re, cp.stderr):
        raise AssertionError(
            f"{what} stderr {cp.stderr!r} does not match {stderr_re!r}"
        )

import os
from pathlib import Path


def read_file_bytes(path: str | Path) -> bytes:
    return Path(path).read_bytes()


def check_mode_recursive(
    path: str | Path, dir_mode: int = 0o700, file_mode: int = 0o600
) -> list[tuple[Path, int]]:
    """Report entries under path whose permission bits differ from expected.

    Returns (entry, actual_mode) pairs; empty means every dir matches dir_mode
    and every file matches file_mode. Symlinks and non-regular files (sockets,
    fifos) are skipped: a mode check is only meaningful for dirs and files.
    """
    root = Path(path)
    violations: list[tuple[Path, int]] = []

    def check(entry: Path, expected: int) -> None:
        actual = os.lstat(entry).st_mode & 0o777
        if actual != expected:
            violations.append((entry, actual))

    check(root, dir_mode)
    for dirpath, dirnames, filenames in os.walk(root):
        base = Path(dirpath)
        for name in dirnames:
            p = base / name
            if p.is_symlink():
                continue
            check(p, dir_mode)
        for name in filenames:
            p = base / name
            if p.is_symlink() or not p.is_file():
                continue
            check(p, file_mode)
    return violations

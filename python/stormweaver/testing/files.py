import os
import re
import struct
from pathlib import Path

_PRINTABLE = re.compile(rb"[\x20-\x7e]{4,}")
PAGE_SIZE = 8192
PD_CHECKSUM_OFFSET = 8


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


def wal_strings(wal_dir: str | Path, pattern: str = "0000000100000000000000??") -> str:
    """Printable runs (>= 4 bytes) of WAL segments matching pattern, like strings(1)."""
    out: list[str] = []
    for seg in sorted(Path(wal_dir).glob(pattern)):
        if not seg.is_file():
            continue
        for m in _PRINTABLE.finditer(seg.read_bytes()):
            out.append(m.group().decode("ascii"))
    return "\n".join(out)


def corrupt_page_checksum(path: str | Path, block: int) -> None:
    """Flip the pd_checksum of one block; mirrors Cluster.pm corrupt_page_checksum."""
    p = Path(path)
    with open(p, "r+b") as f:
        f.seek(block * PAGE_SIZE + PD_CHECKSUM_OFFSET)
        (old,) = struct.unpack("<H", f.read(2))
        f.seek(block * PAGE_SIZE + PD_CHECKSUM_OFFSET)
        f.write(struct.pack("<H", old ^ 0xFFFF))

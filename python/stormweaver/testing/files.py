from pathlib import Path


def read_file_bytes(path: str | Path) -> bytes:
    return Path(path).read_bytes()

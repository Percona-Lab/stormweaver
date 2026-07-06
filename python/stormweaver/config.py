import errno
import random
import socket
import tomllib
from pathlib import Path
from typing import Any


def _bindable(port: int) -> bool:
    # postgres binds both stacks on localhost, so both must be free.
    # a family missing on the host is fine, postgres skips it too.
    for family, addr in ((socket.AF_INET, "127.0.0.1"), (socket.AF_INET6, "::1")):
        try:
            with socket.socket(family) as s:
                s.bind((addr, port))
        except OSError as e:
            if e.errno in (errno.EADDRINUSE, errno.EACCES):
                return False
    return True


class Config:
    def __init__(self, data: dict[str, Any]) -> None:
        self._data = data
        defaults = data.get("default", {})
        self.pgroot: str = defaults.get("pgroot", "")
        self.datadir_root: str = defaults.get("datadir_root", "datadirs")
        self.port_start: int = defaults.get("port_start", 15432)
        self.port_end: int = defaults.get("port_end", 15531)
        self._allocated_ports: set[int] = set()

    @classmethod
    def load(cls, path: str | Path) -> Config:
        with open(path, "rb") as f:
            return cls(tomllib.load(f))

    def datadir(self, name: str) -> str:
        return str(Path(self.datadir_root) / f"datadir_{name}")

    def free_port(self) -> int:
        # random pick avoids collisions with other stormweaver runs on the box
        candidates = [
            p
            for p in range(self.port_start, self.port_end + 1)
            if p not in self._allocated_ports
        ]
        random.shuffle(candidates)
        for port in candidates:
            if _bindable(port):
                self._allocated_ports.add(port)
                return port
        raise RuntimeError(f"no free port in range {self.port_start}-{self.port_end}")

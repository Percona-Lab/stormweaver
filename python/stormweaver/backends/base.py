from abc import ABC, abstractmethod
from pathlib import Path
from typing import Any


class DatabaseBackend(ABC):
    """Server lifecycle for one database node.

    Implementations: Postgres, MySQL.
    """

    datadir: Path

    @abstractmethod
    def initialize(self) -> None: ...

    @abstractmethod
    def add_config(self, settings: dict[str, str]) -> None: ...

    @abstractmethod
    def start(self) -> None: ...

    @abstractmethod
    def stop(self, timeout: float = 10.0) -> None: ...

    @abstractmethod
    def kill(self) -> None: ...

    @abstractmethod
    def restart(self, timeout: float = 10.0) -> None: ...

    @abstractmethod
    def wait_ready(self, timeout: float = 60.0) -> bool: ...

    @abstractmethod
    def createdb(self, name: str) -> None: ...

    @abstractmethod
    def connection_params(self, dbname: str) -> dict[str, Any]: ...

    @abstractmethod
    def is_running(self) -> bool: ...

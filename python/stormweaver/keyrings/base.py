import shutil
from abc import ABC, abstractmethod
from pathlib import Path
from types import TracebackType
from typing import ClassVar, Literal, Self

Scope = Literal["global", "database"]


class RunningService(ABC):
    """Backing service of a keyring; managed=False means hands-off."""

    managed: ClassVar[bool] = True

    @abstractmethod
    def start(self) -> None: ...

    @abstractmethod
    def stop(self) -> None: ...

    def restart(self, *, fresh: bool = False) -> None:
        self.stop()
        if fresh:
            self.wipe_state()
        self.start()

    def wipe_state(self) -> None:  # noqa: B027  default no-op; override to reset state
        pass


class ExternalService(RunningService):
    """Endpoint managed outside stormweaver; lifecycle calls are errors."""

    managed: ClassVar[bool] = False

    def start(self) -> None:
        raise RuntimeError("external keyring service cannot be started")

    def stop(self) -> None:
        raise RuntimeError("external keyring service cannot be stopped")


class Keyring(ABC):
    """A provisioned keyring plus how to connect to it; database-agnostic."""

    kind: ClassVar[str]
    has_service: ClassVar[bool] = True

    def __init__(
        self,
        service: RunningService | None = None,
        workdir: Path | None = None,
        owns_workdir: bool = False,
    ) -> None:
        self._service = service
        self._workdir = workdir
        self._owns_workdir = owns_workdir

    @property
    def managed(self) -> bool:
        return self._service is not None and self._service.managed

    def restart_service(self, *, fresh: bool = False) -> None:
        if self._service is None:
            raise RuntimeError(f"{self.kind} keyring has no backing service")
        self._service.restart(fresh=fresh)

    def close(self) -> None:
        if self._service is not None and self._service.managed:
            self._service.stop()
        if self._owns_workdir and self._workdir is not None:
            shutil.rmtree(self._workdir, ignore_errors=True)

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.close()

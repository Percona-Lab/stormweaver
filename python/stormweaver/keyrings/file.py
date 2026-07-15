from pathlib import Path
from typing import ClassVar

from stormweaver.keyrings.base import Keyring


class FileKeyring(Keyring):
    """pg_tde file provider; no backing service."""

    kind: ClassVar[str] = "file"
    has_service: ClassVar[bool] = False

    def __init__(self, path: str | Path) -> None:
        super().__init__()
        self.path = Path(path)

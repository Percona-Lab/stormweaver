"""PostgreSQL installation discovery: pg_config first, directory guessing second."""

from __future__ import annotations

import glob
import os
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

FALLBACK_GLOBS = (
    "~/.local/share/stormweaver-pg*",
    "/usr/lib/postgresql*",
    "/usr/lib/postgresql/*",
    "/usr/pgsql-*",
)


def _fallback_dirs() -> list[Path]:
    out: list[Path] = []
    for pattern in FALLBACK_GLOBS:
        for hit in sorted(glob.glob(os.path.expanduser(pattern)), reverse=True):
            out.append(Path(hit))
    return out


@dataclass(frozen=True)
class PgInstall:
    pg_config: Path | None
    install_dir: Path
    bindir: Path
    pkglibdir: Path | None
    sharedir: Path | None
    major: int | None

    @staticmethod
    def _query(pg_config: Path, flag: str) -> str:
        cp = subprocess.run(
            [str(pg_config), flag], capture_output=True, text=True, check=True
        )
        return cp.stdout.strip()

    @classmethod
    def from_pg_config(cls, pg_config: str | Path) -> PgInstall:
        pg_config = Path(pg_config)
        bindir = Path(cls._query(pg_config, "--bindir"))
        version = cls._query(pg_config, "--version")
        m = re.search(r"PostgreSQL (\d+)", version)
        return cls(
            pg_config=pg_config,
            install_dir=bindir.parent,
            bindir=bindir,
            pkglibdir=Path(cls._query(pg_config, "--pkglibdir")),
            sharedir=Path(cls._query(pg_config, "--sharedir")),
            major=int(m.group(1)) if m else None,
        )

    @classmethod
    def from_install_dir(cls, install_dir: str | Path) -> PgInstall:
        install_dir = Path(install_dir)
        pg_config = install_dir / "bin" / "pg_config"
        if os.access(pg_config, os.X_OK):
            return cls.from_pg_config(pg_config)
        return cls(
            pg_config=None,
            install_dir=install_dir,
            bindir=install_dir / "bin",
            pkglibdir=None,
            sharedir=None,
            major=None,
        )


def resolve(explicit_pg_config: str | None) -> PgInstall | None:
    """--pg-config, STORMWEAVER_PG_CONFIG, STORMWEAVER_PG_DIR, then known dirs."""
    cand = explicit_pg_config or os.environ.get("STORMWEAVER_PG_CONFIG")
    if cand:
        return PgInstall.from_pg_config(cand)
    env_dir = os.environ.get("STORMWEAVER_PG_DIR")
    if env_dir:
        return PgInstall.from_install_dir(env_dir)
    for d in _fallback_dirs():
        if (d / "bin" / "initdb").exists():
            return PgInstall.from_install_dir(d)
    return None

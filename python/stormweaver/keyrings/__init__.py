import os
from pathlib import Path
from types import ModuleType
from typing import Any

from stormweaver.config import Config
from stormweaver.keyrings import kmip as _kmip
from stormweaver.keyrings import vault as _vault
from stormweaver.keyrings.base import Keyring, Scope
from stormweaver.keyrings.file import FileKeyring
from stormweaver.keyrings.kmip import KmipKeyring
from stormweaver.keyrings.provision import detect_runtime
from stormweaver.keyrings.vault import VaultKeyring

KINDS = ("file", "vault", "kmip")
_SERVICE_MODS: dict[str, ModuleType] = {"vault": _vault, "kmip": _kmip}

__all__ = [
    "KINDS",
    "FileKeyring",
    "Keyring",
    "KmipKeyring",
    "Scope",
    "VaultKeyring",
    "available_keyrings",
    "load_keyring_config",
    "open_keyring",
    "resolve_provision",
    "selected_keyrings",
]


def load_keyring_config(path: str | Path | None = None) -> dict[str, Any]:
    p = Path(path or os.environ.get("STORMWEAVER_CONFIG") or "config/stormweaver.toml")
    if not p.is_file():
        return {}
    return Config.load(p).keyrings


def resolve_provision(kind: str, cfg: dict[str, Any]) -> str | None:
    """Provisioning mode for kind under cfg, or None if unavailable."""
    if kind == "file":
        return "local"
    mode = str(cfg.get("provision", "auto"))
    mod = _SERVICE_MODS[kind]
    runtime = cfg.get("runtime")
    if mode == "auto":
        # external is never auto-picked: touching a shared service must be explicit
        if mod.find_binary(cfg):
            return "executable"
        if detect_runtime(runtime):
            return "container"
        return None
    if mode == "executable":
        return mode if mod.find_binary(cfg) else None
    if mode == "container":
        return mode if detect_runtime(runtime) else None
    if mode == "external":
        return mode
    raise ValueError(f"[keyring.{kind}] unknown provision mode: {mode}")


def available_keyrings(config: dict[str, Any] | None = None) -> list[str]:
    config = load_keyring_config() if config is None else config
    return [k for k in KINDS if resolve_provision(k, config.get(k, {})) is not None]


def selected_keyrings(config: dict[str, Any] | None = None) -> list[str]:
    """Available kinds, or the STORMWEAVER_KEYRINGS set (hard error if short)."""
    config = load_keyring_config() if config is None else config
    avail = available_keyrings(config)
    forced = os.environ.get("STORMWEAVER_KEYRINGS")
    if not forced:
        return avail
    requested = [k.strip() for k in forced.split(",") if k.strip()]
    unknown = [k for k in requested if k not in KINDS]
    if unknown:
        raise RuntimeError(
            f"STORMWEAVER_KEYRINGS: unknown keyring(s): {', '.join(unknown)}"
        )
    missing = [k for k in requested if k not in avail]
    if missing:
        raise RuntimeError(
            f"STORMWEAVER_KEYRINGS: keyring(s) not available: {', '.join(missing)}"
        )
    return requested


def open_keyring(
    kind: str, workdir: str | Path, config: dict[str, Any] | None = None
) -> Keyring:
    """Provision if needed and return a keyring; caller closes it (ctx manager)."""
    config = load_keyring_config() if config is None else config
    wd = Path(workdir)
    wd.mkdir(parents=True, exist_ok=True)
    if kind == "file":
        return FileKeyring(wd / "keyring.per")
    if kind not in KINDS:
        raise RuntimeError(f"unknown keyring kind: {kind}")
    cfg = config.get(kind, {})
    mode = resolve_provision(kind, cfg)
    if mode is None:
        raise RuntimeError(
            f"keyring {kind!r} is not available (no binary or container runtime; "
            f"see [keyring.{kind}] in stormweaver.toml)"
        )
    mod = _SERVICE_MODS[kind]
    if mode == "executable":
        return mod.open_executable(cfg, wd)  # type: ignore[no-any-return]
    if mode == "container":
        runtime = detect_runtime(cfg.get("runtime"))
        assert runtime is not None
        return mod.open_container(cfg, wd, runtime)  # type: ignore[no-any-return]
    return mod.open_external(cfg, wd)  # type: ignore[no-any-return]

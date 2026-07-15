import json
import os
import shutil
from pathlib import Path
from typing import Any, ClassVar

from stormweaver.config import alloc_port
from stormweaver.keyrings import provision
from stormweaver.keyrings.base import ExternalService, Keyring, RunningService

DEFAULT_IMAGE = "openbao/openbao:2.5.4"
DEFAULT_BIN = "bao"
CONTAINER_PORT = 8200
# pinned so restarts don't invalidate the token file
DEV_ROOT_TOKEN = "stormweaver-root"


class VaultKeyring(Keyring):
    kind: ClassVar[str] = "vault"

    def __init__(
        self,
        *,
        url: str,
        mount_path: str,
        token_file: Path,
        ca_cert: Path,
        namespace: str = "",
        service: RunningService | None = None,
    ) -> None:
        super().__init__(service)
        self.url = url
        self.mount_path = mount_path
        self.token_file = token_file
        self.ca_cert = ca_cert
        self.namespace = namespace


def find_binary(cfg: dict[str, Any]) -> str | None:
    cand = os.environ.get("OPENBAO_BIN") or str(cfg.get("bin", DEFAULT_BIN))
    if "/" in cand:
        return cand if os.access(cand, os.X_OK) else None
    return shutil.which(cand)


def open_executable(cfg: dict[str, Any], workdir: Path) -> VaultKeyring:
    binary = find_binary(cfg)
    if binary is None:
        raise RuntimeError("bao binary not found")
    port = alloc_port()
    cluster_json = workdir / "cluster.json"
    svc = provision.ProcessService(
        [
            binary,
            "server",
            "-dev",
            "-dev-tls",
            f"-dev-listen-address=127.0.0.1:{port}",
            f"-dev-root-token-id={DEV_ROOT_TOKEN}",
            # cert dir pinned to workdir: path stays valid across restarts
            f"-dev-tls-cert-dir={workdir}",
            f"-dev-cluster-json={cluster_json}",
        ],
        what="openbao (executable)",
        log_path=workdir / "bao.log",
        ready=provision.file_ready(cluster_json),
        pre_start=[cluster_json],
    )
    svc.start()
    info = json.loads(cluster_json.read_text())
    token_file = workdir / "token"
    token_file.write_text(DEV_ROOT_TOKEN)
    return VaultKeyring(
        url=f"https://127.0.0.1:{port}",
        mount_path="secret",
        token_file=token_file,
        ca_cert=Path(str(info["ca_cert_path"])),
        service=svc,
    )


def open_container(cfg: dict[str, Any], workdir: Path, runtime: str) -> VaultKeyring:
    port = alloc_port()
    out = workdir / "out"
    out.mkdir(exist_ok=True)
    # container user writes cluster json + tls certs here
    out.chmod(0o777)
    cluster_json = out / "cluster.json"
    svc = provision.ContainerService(
        runtime,
        name=provision.container_name("vault"),
        image=str(cfg.get("image", DEFAULT_IMAGE)),
        what="openbao (container)",
        cmd=[
            "server",
            "-dev",
            "-dev-tls",
            f"-dev-listen-address=0.0.0.0:{CONTAINER_PORT}",
            f"-dev-root-token-id={DEV_ROOT_TOKEN}",
            "-dev-tls-cert-dir=/out",
            "-dev-cluster-json=/out/cluster.json",
        ],
        mounts=[(out, "/out")],
        # container writes cluster.json + tls certs here; host reads them back
        world_readable=["/out"],
        ports={port: CONTAINER_PORT},
        ready=provision.file_ready(cluster_json),
        pre_start=[cluster_json],
    )
    svc.start()
    info = json.loads(cluster_json.read_text())
    token_file = workdir / "token"
    token_file.write_text(DEV_ROOT_TOKEN)
    return VaultKeyring(
        url=f"https://127.0.0.1:{port}",
        mount_path="secret",
        token_file=token_file,
        # json paths are container paths; the file itself is in the mount
        ca_cert=out / Path(str(info["ca_cert_path"])).name,
        service=svc,
    )


def open_external(cfg: dict[str, Any], workdir: Path) -> VaultKeyring:
    required = ("url", "token_file", "ca_cert")
    missing = [k for k in required if not cfg.get(k)]
    if missing:
        raise RuntimeError(
            f"[keyring.vault] external mode missing: {', '.join(missing)}"
        )
    provision.check_reachable(str(cfg["url"]))
    return VaultKeyring(
        url=str(cfg["url"]),
        mount_path=str(cfg.get("mount_path", "secret")),
        token_file=Path(str(cfg["token_file"])),
        ca_cert=Path(str(cfg["ca_cert"])),
        namespace=str(cfg.get("namespace", "")),
        service=ExternalService(),
    )

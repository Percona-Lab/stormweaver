import os
import shutil
from pathlib import Path
from typing import Any, ClassVar

from stormweaver.config import alloc_port
from stormweaver.keyrings import certs, provision
from stormweaver.keyrings.base import ExternalService, Keyring, RunningService

DEFAULT_IMAGE = "ghcr.io/cosmian/kms:5.21.0"
DEFAULT_BIN = "cosmian_kms"
CONTAINER_KMIP_PORT = 5696
CONTAINER_HTTP_PORT = 9998

# cosmian needs OPENSSL_MODULES; common locations, first hit wins
_OSSL_MODULE_DIRS = (
    "/usr/local/cosmian/lib/ossl-modules",
    "/usr/lib64/ossl-modules",
    "/usr/lib/x86_64-linux-gnu/ossl-modules",
    "/usr/lib/aarch64-linux-gnu/ossl-modules",
)


class KmipKeyring(Keyring):
    kind: ClassVar[str] = "kmip"

    def __init__(
        self,
        *,
        host: str,
        port: int,
        client_cert: Path,
        client_key: Path,
        ca_cert: Path,
        service: RunningService | None = None,
    ) -> None:
        super().__init__(service)
        self.host = host
        self.port = port
        self.client_cert = client_cert
        self.client_key = client_key
        self.ca_cert = ca_cert


def find_binary(cfg: dict[str, Any]) -> str | None:
    cand = os.environ.get("COSMIAN_KMS_BIN") or str(cfg.get("bin", DEFAULT_BIN))
    if "/" in cand:
        return cand if os.access(cand, os.X_OK) else None
    return shutil.which(cand)


def _kms_toml(data: str, host: str, kmip_port: int, http_port: int) -> str:
    return f"""default_username = "admin"

[db]
database_type = "sqlite"
sqlite_path   = "{data}/db"
clear_database = true

[tls]
tls_p12_file         = "{data}/server.p12"
tls_p12_password     = "test"
clients_ca_cert_file = "{data}/ca.pem"

[socket_server]
socket_server_start    = true
socket_server_port     = {kmip_port}
socket_server_hostname = "{host}"

[http]
port     = {http_port}
hostname = "{host}"

[logging]
rust_log = "info,cosmian_kms=info"
"""


def open_executable(cfg: dict[str, Any], workdir: Path) -> KmipKeyring:
    binary = find_binary(cfg)
    if binary is None:
        raise RuntimeError("cosmian_kms binary not found")
    data = workdir / "data"
    certs.gen_kmip_certs(data)
    kmip_port = alloc_port()
    http_port = alloc_port()
    toml = workdir / "kms.toml"
    toml.write_text(_kms_toml(str(data), "127.0.0.1", kmip_port, http_port))
    env: dict[str, str] = {}
    if not os.environ.get("OPENSSL_MODULES"):
        for d in _OSSL_MODULE_DIRS:
            # need a legacy.so we can actually read; a bundled one may be root-only
            if os.access(os.path.join(d, "legacy.so"), os.R_OK):
                env["OPENSSL_MODULES"] = d
                break
    svc = provision.ProcessService(
        [binary, "-c", toml],
        what="cosmian_kms (executable)",
        log_path=workdir / "kms.log",
        ready=provision.http_ready(f"https://127.0.0.1:{http_port}/version"),
        env=env,
        fresh_paths=[data / "db"],
    )
    svc.start()
    return KmipKeyring(
        host="127.0.0.1",
        port=kmip_port,
        client_cert=data / "client.pem",
        client_key=data / "client.key",
        ca_cert=data / "ca.pem",
        service=svc,
    )


def open_container(cfg: dict[str, Any], workdir: Path, runtime: str) -> KmipKeyring:
    data = workdir / "data"
    certs.gen_kmip_certs(data)
    (data / "kms.toml").write_text(
        _kms_toml("/data", "0.0.0.0", CONTAINER_KMIP_PORT, CONTAINER_HTTP_PORT)
    )
    # container user must read certs and write the sqlite db
    data.chmod(0o777)
    for f in data.iterdir():
        f.chmod(0o644)
    kmip_port = alloc_port()
    http_port = alloc_port()
    svc = provision.ContainerService(
        runtime,
        name=provision.container_name("kmip"),
        image=str(cfg.get("image", DEFAULT_IMAGE)),
        what="cosmian_kms (container)",
        cmd=["-c", "/data/kms.toml"],
        mounts=[(data, "/data")],
        ports={kmip_port: CONTAINER_KMIP_PORT, http_port: CONTAINER_HTTP_PORT},
        ready=provision.http_ready(f"https://127.0.0.1:{http_port}/version"),
        fresh_paths=[data / "db"],
    )
    svc.start()
    return KmipKeyring(
        host="127.0.0.1",
        port=kmip_port,
        client_cert=data / "client.pem",
        client_key=data / "client.key",
        ca_cert=data / "ca.pem",
        service=svc,
    )


def open_external(cfg: dict[str, Any], workdir: Path) -> KmipKeyring:
    required = ("host", "port", "client_cert", "client_key", "ca_cert")
    missing = [k for k in required if not cfg.get(k)]
    if missing:
        raise RuntimeError(
            f"[keyring.kmip] external mode missing: {', '.join(missing)}"
        )
    provision.check_reachable(f"//{cfg['host']}:{cfg['port']}")
    return KmipKeyring(
        host=str(cfg["host"]),
        port=int(cfg["port"]),
        client_cert=Path(str(cfg["client_cert"])),
        client_key=Path(str(cfg["client_key"])),
        ca_cert=Path(str(cfg["ca_cert"])),
        service=ExternalService(),
    )

import subprocess
from pathlib import Path


def _openssl(*args: str) -> None:
    cp = subprocess.run(["openssl", *args], capture_output=True, text=True)
    if cp.returncode != 0:
        raise RuntimeError(f"openssl {args[0]} failed: {cp.stderr}")


def gen_kmip_certs(dir: Path) -> None:
    """Throwaway CA + server (pem/p12) + client certs for a localhost KMIP server."""
    dir.mkdir(parents=True, exist_ok=True)
    d = str(dir)
    _openssl(
        "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
        "-keyout", f"{d}/ca.key", "-out", f"{d}/ca.pem",
        "-subj", "/CN=stormweaver-test-ca",
    )  # fmt: skip
    _openssl(
        "req", "-newkey", "rsa:2048", "-nodes",
        "-keyout", f"{d}/server.key", "-out", f"{d}/server.csr",
        "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
    )  # fmt: skip
    _openssl(
        "x509", "-req", "-in", f"{d}/server.csr", "-CA", f"{d}/ca.pem",
        "-CAkey", f"{d}/ca.key", "-CAcreateserial", "-days", "1",
        "-out", f"{d}/server.pem", "-copy_extensions", "copy",
    )  # fmt: skip
    _openssl(
        "pkcs12", "-export", "-out", f"{d}/server.p12",
        "-inkey", f"{d}/server.key", "-in", f"{d}/server.pem",
        "-password", "pass:test",
    )  # fmt: skip
    _openssl(
        "req", "-newkey", "rsa:2048", "-nodes",
        "-keyout", f"{d}/client.key", "-out", f"{d}/client.csr",
        "-subj", "/CN=stormweaver-client",
    )  # fmt: skip
    _openssl(
        "x509", "-req", "-in", f"{d}/client.csr", "-CA", f"{d}/ca.pem",
        "-CAkey", f"{d}/ca.key", "-CAcreateserial", "-days", "1",
        "-out", f"{d}/client.pem",
    )  # fmt: skip

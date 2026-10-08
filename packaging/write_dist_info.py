"""Write <site-packages>/stormweaver-<version>.dist-info after a plain CMake install.

pip and importlib.metadata need it to see the package (entry points, version).
"""

import base64
import hashlib
import sys
import tomllib
from pathlib import Path


def write_dist_info(site_packages: Path, source_dir: Path) -> Path:
    proj = tomllib.loads((source_dir / "pyproject.toml").read_text(encoding="utf-8"))[
        "project"
    ]
    version = (source_dir / "VERSION").read_text(encoding="utf-8").strip()
    dist_info = site_packages / f"{proj['name']}-{version}.dist-info"
    dist_info.mkdir(parents=True, exist_ok=True)

    metadata = [
        "Metadata-Version: 2.1",
        f"Name: {proj['name']}",
        f"Version: {version}",
        f"Summary: {proj['description']}",
        f"Requires-Python: {proj['requires-python']}",
    ]
    metadata.extend(f"Requires-Dist: {dep}" for dep in proj.get("dependencies", []))
    metadata.append("")
    (dist_info / "METADATA").write_text("\n".join(metadata), encoding="utf-8")

    groups = {"console_scripts": proj.get("scripts", {})}
    groups.update(proj.get("entry-points", {}))
    lines: list[str] = []
    for group, entries in groups.items():
        if not entries:
            continue
        lines.append(f"[{group}]")
        lines.extend(f"{k} = {v}" for k, v in entries.items())
        lines.append("")
    (dist_info / "entry_points.txt").write_text("\n".join(lines), encoding="utf-8")
    (dist_info / "INSTALLER").write_text("cmake\n", encoding="utf-8")
    (dist_info / "top_level.txt").write_text(f"{proj['name']}\n", encoding="utf-8")

    files = sorted(p for p in (site_packages / proj["name"]).rglob("*") if p.is_file())
    files += sorted(p for p in dist_info.iterdir() if p.name != "RECORD")
    record = []
    for p in files:
        # packagers strip the .so after install, a hash would only go stale
        if p.suffix == ".so":
            record.append(f"{p.relative_to(site_packages)},,")
            continue
        digest = base64.urlsafe_b64encode(hashlib.sha256(p.read_bytes()).digest())
        record.append(
            f"{p.relative_to(site_packages)},sha256={digest.rstrip(b'=').decode()},{p.stat().st_size}"
        )
    record.append(f"{dist_info.name}/RECORD,,")
    (dist_info / "RECORD").write_text("\n".join(record) + "\n", encoding="utf-8")
    return dist_info


if __name__ == "__main__":
    write_dist_info(Path(sys.argv[1]), Path(sys.argv[2]))

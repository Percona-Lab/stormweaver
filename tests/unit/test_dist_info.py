import importlib.metadata
import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _load():
    spec = importlib.util.spec_from_file_location(
        "write_dist_info", REPO / "packaging" / "write_dist_info.py"
    )
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


def test_dist_info_is_discoverable(tmp_path):
    src = tmp_path / "src"
    src.mkdir()
    (src / "VERSION").write_text("9.8.7\n")
    (src / "pyproject.toml").write_text(
        '[project]\nname = "stormweaver"\ndescription = "d"\n'
        'requires-python = ">=3.14"\ndependencies = ["pytest>=8"]\n'
        '[project.scripts]\nstormweaver = "stormweaver.cli:main"\n'
        '[project.entry-points.pytest11]\nstormweaver = "stormweaver.pytest_plugin"\n'
    )
    site = tmp_path / "site"
    (site / "stormweaver").mkdir(parents=True)
    (site / "stormweaver" / "__init__.py").write_text("x = 1\n")
    so = site / "stormweaver" / "_stormweaver.cpython-314t-x86_64-linux-gnu.so"
    so.write_bytes(b"\x7fELF\x00\x01")

    dist_info = _load().write_dist_info(site, src)

    dist = importlib.metadata.Distribution.at(dist_info)
    assert dist.version == "9.8.7"
    eps = {(e.group, e.name, e.value) for e in dist.entry_points}
    assert ("pytest11", "stormweaver", "stormweaver.pytest_plugin") in eps
    assert ("console_scripts", "stormweaver", "stormweaver.cli:main") in eps
    assert dist.requires == ["pytest>=8"]
    record = (dist_info / "RECORD").read_text()
    lines = dict(line.split(",", 1) for line in record.splitlines())
    digest, _size = lines["stormweaver/__init__.py"].split(",")
    assert digest.startswith("sha256=")
    assert len(digest.removeprefix("sha256=")) == 43
    assert "=" not in digest.removeprefix("sha256=")
    assert lines[so.relative_to(site).as_posix()] == ","
    assert record.rstrip().endswith("RECORD,,")

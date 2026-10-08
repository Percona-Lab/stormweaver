import os
import stat
from pathlib import Path

from stormweaver import pginstall


def _fake_pg_config(tmp_path: Path, prefix: Path, major: int = 18) -> Path:
    (prefix / "bin").mkdir(parents=True)
    (prefix / "bin" / "initdb").touch()
    script = tmp_path / "pg_config"
    script.write_text(
        "#!/bin/sh\n"
        'case "$1" in\n'
        f'  --bindir) echo "{prefix}/bin";;\n'
        f'  --pkglibdir) echo "{prefix}/lib64/postgresql";;\n'
        f'  --sharedir) echo "{prefix}/share/postgresql";;\n'
        f'  --version) echo "PostgreSQL {major}.4 - Percona Server for '
        f'PostgreSQL {major}.4.1";;\n'
        "  *) exit 1;;\n"
        "esac\n"
    )
    script.chmod(script.stat().st_mode | stat.S_IEXEC)
    return script


def test_from_pg_config_derives_dirs(tmp_path):
    prefix = tmp_path / "inst"
    cfg = _fake_pg_config(tmp_path, prefix)
    pi = pginstall.PgInstall.from_pg_config(cfg)
    assert pi.bindir == prefix / "bin"
    assert pi.pkglibdir == prefix / "lib64" / "postgresql"
    assert pi.sharedir == prefix / "share" / "postgresql"
    assert pi.install_dir == prefix
    assert pi.major == 18


def test_from_pg_config_beta_version(tmp_path):
    prefix = tmp_path / "inst"
    cfg = _fake_pg_config(tmp_path, prefix)
    cfg.write_text(cfg.read_text().replace("18.4 - Percona", "19beta4 - Percona"))
    assert pginstall.PgInstall.from_pg_config(cfg).major == 19


def test_resolve_prefers_explicit_then_env(tmp_path, monkeypatch):
    prefix = tmp_path / "a"
    cfg = _fake_pg_config(tmp_path, prefix)
    monkeypatch.setenv("STORMWEAVER_PG_CONFIG", str(cfg))
    monkeypatch.delenv("STORMWEAVER_PG_DIR", raising=False)
    assert pginstall.resolve(None).install_dir == prefix
    other = tmp_path / "b"
    (other / "bin").mkdir(parents=True)
    (other / "bin" / "initdb").touch()
    monkeypatch.setenv("STORMWEAVER_PG_DIR", str(other))
    # pg_config wins over PG_DIR
    assert pginstall.resolve(None).install_dir == prefix
    monkeypatch.delenv("STORMWEAVER_PG_CONFIG")
    assert pginstall.resolve(None).install_dir == other


def test_resolve_from_install_dir_uses_its_pg_config(tmp_path, monkeypatch):
    prefix = tmp_path / "inst"
    cfg = _fake_pg_config(tmp_path, prefix)
    os.rename(cfg, prefix / "bin" / "pg_config")
    monkeypatch.delenv("STORMWEAVER_PG_CONFIG", raising=False)
    monkeypatch.setenv("STORMWEAVER_PG_DIR", str(prefix))
    pi = pginstall.resolve(None)
    assert pi.major == 18
    assert pi.pkglibdir == prefix / "lib64" / "postgresql"


def test_resolve_none_when_nothing(tmp_path, monkeypatch):
    monkeypatch.delenv("STORMWEAVER_PG_CONFIG", raising=False)
    monkeypatch.delenv("STORMWEAVER_PG_DIR", raising=False)
    monkeypatch.setattr(pginstall, "_fallback_dirs", lambda: [tmp_path / "nope"])
    assert pginstall.resolve(None) is None


def test_fallback_globs_include_rpm_layout():
    assert "/usr/pgsql-*" in pginstall.FALLBACK_GLOBS
    assert "/usr/lib/postgresql/*" in pginstall.FALLBACK_GLOBS

import stat

import pytest
from stormweaver.testing import process


def _fake_tool(tmp_path, name="pg_fake", version_line="pg_fake (PostgreSQL) 18.4"):
    bindir = tmp_path / "bin"
    bindir.mkdir(exist_ok=True)
    exe = bindir / name
    exe.write_text(
        "#!/bin/sh\n"
        'case "$1" in\n'
        f'  --help|-\\?) echo "{name} does things."; echo "Usage:"; exit 0;;\n'
        f'  --version|-V) echo "{version_line}"; exit 0;;\n'
        f'  *) echo "{name}: unrecognized option" >&2;'
        f" echo 'Try \"{name} --help\" for more information.' >&2; exit 1;;\n"
        "esac\n"
    )
    exe.chmod(exe.stat().st_mode | stat.S_IEXEC)
    return tmp_path


def test_program_checks_pass(tmp_path):
    inst = _fake_tool(tmp_path)
    process.program_help_ok(inst, "pg_fake")
    process.program_version_ok(inst, "pg_fake")
    process.program_options_handling_ok(inst, "pg_fake")


def test_program_version_mismatch(tmp_path):
    inst = _fake_tool(tmp_path, version_line="something else")
    with pytest.raises(AssertionError):
        process.program_version_ok(inst, "pg_fake")


def test_program_version_accepts_upstream_name(tmp_path):
    inst = _fake_tool(tmp_path, name="pg_fake")
    process.program_version_ok(inst, "pg_fake")


def _bare_tool(tmp_path, body):
    bindir = tmp_path / "bin"
    bindir.mkdir(exist_ok=True)
    exe = bindir / "pg_bare"
    exe.write_text(f"#!/bin/sh\n{body}\n")
    exe.chmod(exe.stat().st_mode | stat.S_IEXEC)
    return tmp_path


def test_program_options_without_hint(tmp_path):
    inst = _bare_tool(tmp_path, "echo 'error: bad option' >&2; exit 1")
    process.program_options_handling_ok(inst, "pg_bare")


def test_program_options_accepting_fails(tmp_path):
    inst = _bare_tool(tmp_path, "exit 0")
    with pytest.raises(AssertionError):
        process.program_options_handling_ok(inst, "pg_bare")

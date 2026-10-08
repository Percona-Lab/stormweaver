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

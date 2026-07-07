import subprocess
import sys
import textwrap

import pytest
import stormweaver.testing as st


def _write_standalone(path, body):
    path.write_text(textwrap.dedent(body))


def test_standalone_runner_pass(tmp_path):
    f = tmp_path / "test_standalone_ok.py"
    _write_standalone(
        f,
        """
        import stormweaver.testing

        def test_trivial():
            assert 1 + 1 == 2

        if __name__ == "__main__":
            stormweaver.testing.main()
        """,
    )
    cp = subprocess.run(
        [sys.executable, str(f)], capture_output=True, text=True, cwd=tmp_path
    )
    assert cp.returncode == 0, cp.stdout + cp.stderr


def test_standalone_runner_fail(tmp_path):
    f = tmp_path / "test_standalone_bad.py"
    _write_standalone(
        f,
        """
        import stormweaver.testing

        def test_broken():
            assert False

        if __name__ == "__main__":
            stormweaver.testing.main()
        """,
    )
    cp = subprocess.run(
        [sys.executable, str(f)], capture_output=True, text=True, cwd=tmp_path
    )
    assert cp.returncode != 0


def test_discovery_helpers(monkeypatch, tmp_path):
    assert callable(st.mysql_install_dir)
    # env var wins verbatim: _detect_pg_dir does not validate its contents
    monkeypatch.setenv("STORMWEAVER_PG_DIR", str(tmp_path))
    assert st.pg_install_dir() == str(tmp_path)


def test_pg_install_dir_not_found(monkeypatch):
    import stormweaver.pytest_plugin as plugin

    monkeypatch.setattr(plugin, "_detect_pg_dir", lambda: None)
    with pytest.raises(RuntimeError, match="STORMWEAVER_PG_DIR"):
        st.pg_install_dir()

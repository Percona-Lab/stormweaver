import stormweaver.testing as st


def test_check_mode_recursive_clean(tmp_path):
    d = tmp_path / "data"
    d.mkdir()
    d.chmod(0o700)
    sub = d / "sub"
    sub.mkdir()
    sub.chmod(0o700)
    f = sub / "file"
    f.write_text("x")
    f.chmod(0o600)
    assert st.check_mode_recursive(d) == []


def test_check_mode_recursive_reports_violations(tmp_path):
    d = tmp_path / "data"
    d.mkdir()
    d.chmod(0o700)
    bad_file = d / "loose"
    bad_file.write_text("x")
    bad_file.chmod(0o644)
    bad_dir = d / "opendir"
    bad_dir.mkdir()
    bad_dir.chmod(0o755)
    violations = dict(st.check_mode_recursive(d))
    assert violations[bad_file] == 0o644
    assert violations[bad_dir] == 0o755


def test_check_mode_recursive_skips_symlinked_dir(tmp_path):
    d = tmp_path / "data"
    d.mkdir()
    d.chmod(0o700)
    (d / "link").symlink_to(tmp_path)
    assert st.check_mode_recursive(d) == []


def test_check_mode_recursive_skips_symlinked_file(tmp_path):
    d = tmp_path / "data"
    d.mkdir()
    d.chmod(0o700)
    # target mode would be flagged if the helper followed the link
    target = tmp_path / "outside.txt"
    target.write_text("x")
    target.chmod(0o644)
    (d / "flink").symlink_to(target)
    assert st.check_mode_recursive(d) == []


def test_check_mode_recursive_skips_fifo(tmp_path):
    import os

    d = tmp_path / "data"
    d.mkdir()
    d.chmod(0o700)
    # a non-regular entry with a mode that would be flagged if not skipped
    os.mkfifo(d / "fifo", 0o644)
    assert st.check_mode_recursive(d) == []

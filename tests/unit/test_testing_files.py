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


def test_wal_strings_extracts_printable_runs(tmp_path):
    wal = tmp_path / "pg_wal"
    wal.mkdir()
    (wal / "000000010000000000000001").write_bytes(
        b"\x00\x01foobar_plain\x02\x03ab\x00"
    )
    (wal / "000000010000000000000002").write_bytes(
        b"zzzz" + b"\xff" * 10 + b"foobar_enc"
    )
    (wal / "archive_status").mkdir()
    out = st.wal_strings(wal)
    assert "foobar_plain" in out
    assert "foobar_enc" in out
    assert "zzzz" in out
    assert "ab" not in out.split()


def test_corrupt_page_checksum(tmp_path):
    rel = tmp_path / "16384"
    rel.write_bytes(b"\x00" * 8192 * 2)
    st.corrupt_page_checksum(rel, 1)
    data = rel.read_bytes()
    assert data[: 8192 + 8] == b"\x00" * (8192 + 8)
    assert data[8192 + 8 : 8192 + 10] == b"\xff\xff"
    assert data[8192 + 10 :] == b"\x00" * (8192 - 10)

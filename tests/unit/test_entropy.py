import logging

import pytest
from stormweaver.entropy import (
    EncryptionMismatchError,
    calculate_entropy,
    verify_entropy,
)


def _write(tmp_path, name, data):
    p = tmp_path / name
    p.write_bytes(data)
    return str(p)


def test_entropy_constant_file_is_zero(tmp_path):
    path = _write(tmp_path, "zeros", b"\x00" * 8192)
    assert calculate_entropy(path) == 0.0


def test_entropy_uniform_file_is_one(tmp_path):
    path = _write(tmp_path, "uniform", bytes(range(256)) * 32)
    assert calculate_entropy(path) == pytest.approx(1.0)


def test_entropy_empty_file_is_zero(tmp_path):
    path = _write(tmp_path, "empty", b"")
    assert calculate_entropy(path) == 0.0


def test_tde_heap_not_encrypted_raises(tmp_path):
    path = _write(tmp_path, "f", bytes(range(256)) * 32)
    with pytest.raises(EncryptionMismatchError):
        verify_entropy("rel", "tab", "tde_heap", False, path)


def test_heap_encrypted_raises(tmp_path):
    path = _write(tmp_path, "f", b"\x00" * 8192)
    with pytest.raises(EncryptionMismatchError):
        verify_entropy("rel", "tab", "heap", True, path)


def test_tde_heap_encrypted_low_entropy_warns(tmp_path, caplog):
    path = _write(tmp_path, "f", b"\x00\x01" * 4096)  # entropy > 0 but << 0.8
    with caplog.at_level(logging.WARNING, logger="stormweaver.entropy"):
        verify_entropy("rel", "tab", "tde_heap", True, path)
    assert any("too low" in r.message for r in caplog.records)


def test_heap_high_entropy_warns(tmp_path, caplog):
    path = _write(tmp_path, "f", bytes(range(256)) * 32)
    with caplog.at_level(logging.WARNING, logger="stormweaver.entropy"):
        verify_entropy("rel", "tab", "heap", False, path)
    assert any("high entropy" in r.message for r in caplog.records)


def test_consistent_cases_pass(tmp_path):
    encrypted = _write(tmp_path, "enc", bytes(range(256)) * 32)
    plain = _write(tmp_path, "plain", b"\x00" * 8192)
    verify_entropy("rel", "tab", "tde_heap", True, encrypted)
    verify_entropy("rel", "tab", "heap", False, plain)
